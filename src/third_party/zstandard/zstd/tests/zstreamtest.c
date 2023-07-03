/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


/*-************************************
 *  Compiler specific
 **************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  define _CRT_SECURE_NO_WARNINGS   /* fgets */
#  pragma warning(disable : 4127)   /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4146)   /* disable: C4146: minus unsigned expression */
#endif


/*-************************************
 *  Includes
 **************************************/
#include <stdlib.h>       /* free */
#include <stdio.h>        /* fgets, sscanf */
#include <string.h>       /* strcmp */
#include <time.h>         /* time_t, time(), to randomize seed */
#include <assert.h>       /* assert */
#include "timefn.h"       /* UTIL_time_t, UTIL_getTime */
#include "mem.h"
#define ZSTD_DISABLE_DEPRECATE_WARNINGS /* No deprecation warnings, we still test some deprecated functions */
#define ZSTD_STATIC_LINKING_ONLY  /* ZSTD_maxCLevel, ZSTD_customMem, ZSTD_getDictID_fromFrame */
#include "zstd.h"         /* ZSTD_compressBound */
#include "zstd_errors.h"  /* ZSTD_error_srcSize_wrong */
#include "zdict.h"        /* ZDICT_trainFromBuffer */
#include "datagen.h"      /* RDG_genBuffer */
#define XXH_STATIC_LINKING_ONLY   /* XXH64_state_t */
#include "xxhash.h"       /* XXH64_* */
#include "seqgen.h"
#include "util.h"
#include "timefn.h"       /* UTIL_time_t, UTIL_clockSpanMicro, UTIL_getTime */
#include "external_matchfinder.h"   /* zstreamSequenceProducer, EMF_testCase */

/*-************************************
 *  Constants
 **************************************/
#define KB *(1U<<10)
#define MB *(1U<<20)
#define GB *(1U<<30)

static const int nbTestsDefault = 10000;
static const U32 g_cLevelMax_smallTests = 10;
#define COMPRESSIBLE_NOISE_LENGTH (10 MB)
#define FUZ_COMPRESSIBILITY_DEFAULT 50
static const U32 prime32 = 2654435761U;


/*-************************************
 *  Display Macros
 **************************************/
#define DISPLAY(...)          fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...)  if (g_displayLevel>=l) {                     \
                                  DISPLAY(__VA_ARGS__);                    \
                                  if (g_displayLevel>=4) fflush(stderr); }
static U32 g_displayLevel = 2;

static const U64 g_refreshRate = SEC_TO_MICRO / 6;
static UTIL_time_t g_displayClock = UTIL_TIME_INITIALIZER;

#define DISPLAYUPDATE(l, ...) if (g_displayLevel>=l) { \
            if ((UTIL_clockSpanMicro(g_displayClock) > g_refreshRate) || (g_displayLevel>=4)) \
            { g_displayClock = UTIL_getTime(); DISPLAY(__VA_ARGS__); \
            if (g_displayLevel>=4) fflush(stderr); } }

static U64 g_clockTime = 0;


/*-*******************************************************
 *  Check macros
 *********************************************************/
#undef MIN
#undef MAX
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
/*! FUZ_rand() :
    @return : a 27 bits random value, from a 32-bits `seed`.
    `seed` is also modified */
#define FUZ_rotl32(x,r) ((x << r) | (x >> (32 - r)))
static U32 FUZ_rand(U32* seedPtr)
{
    static const U32 prime2 = 2246822519U;
    U32 rand32 = *seedPtr;
    rand32 *= prime32;
    rand32 += prime2;
    rand32  = FUZ_rotl32(rand32, 13);
    *seedPtr = rand32;
    return rand32 >> 5;
}

#define CHECK(cond, ...) {                                   \
    if (cond) {                                              \
        DISPLAY("Error => ");                                \
        DISPLAY(__VA_ARGS__);                                \
        DISPLAY(" (seed %u, test nb %u, line %u) \n",        \
                (unsigned)seed, testNb, __LINE__);           \
        goto _output_error;                                  \
}   }

#define CHECK_Z(f) {                                         \
    size_t const err = f;                                    \
    CHECK(ZSTD_isError(err), "%s : %s ",                     \
          #f, ZSTD_getErrorName(err));                       \
}

#define CHECK_RET(ret, cond, ...) {                          \
    if (cond) {                                              \
        DISPLAY("Error %llu => ", (unsigned long long)ret);  \
        DISPLAY(__VA_ARGS__);                                \
        DISPLAY(" (line %u)\n", __LINE__);                   \
        return ret;                                          \
}   }

#define CHECK_RET_Z(f) {                                     \
    size_t const err = f;                                    \
    CHECK_RET(err, ZSTD_isError(err), "%s : %s ",            \
          #f, ZSTD_getErrorName(err));                       \
}


/*======================================================
 *   Basic Unit tests
 *======================================================*/

typedef struct {
    void* start;
    size_t size;
    size_t filled;
} buffer_t;

static const buffer_t kBuffNull = { NULL, 0 , 0 };

static void FUZ_freeDictionary(buffer_t dict)
{
    free(dict.start);
}

static buffer_t FUZ_createDictionary(const void* src, size_t srcSize, size_t blockSize, size_t requestedDictSize)
{
    buffer_t dict = kBuffNull;
    size_t const nbBlocks = (srcSize + (blockSize-1)) / blockSize;
    size_t* const blockSizes = (size_t*)malloc(nbBlocks * sizeof(size_t));
    if (!blockSizes) return kBuffNull;
    dict.start = malloc(requestedDictSize);
    if (!dict.start) { free(blockSizes); return kBuffNull; }
    {   size_t nb;
        for (nb=0; nb<nbBlocks-1; nb++) blockSizes[nb] = blockSize;
        blockSizes[nbBlocks-1] = srcSize - (blockSize * (nbBlocks-1));
    }
    {   size_t const dictSize = ZDICT_trainFromBuffer(dict.start, requestedDictSize, src, blockSizes, (unsigned)nbBlocks);
        free(blockSizes);
        if (ZDICT_isError(dictSize)) { FUZ_freeDictionary(dict); return kBuffNull; }
        dict.size = requestedDictSize;
        dict.filled = dictSize;
        return dict;
    }
}

/* Round trips data and updates xxh with the decompressed data produced */
static size_t SEQ_roundTrip(ZSTD_CCtx* cctx, ZSTD_DCtx* dctx,
                            XXH64_state_t* xxh, void* data, size_t size,
                            ZSTD_EndDirective endOp)
{
    static BYTE compressed[1024];
    static BYTE uncompressed[1024];

    ZSTD_inBuffer cin = {data, size, 0};
    size_t cret;

    do {
        ZSTD_outBuffer cout = { compressed, sizeof(compressed), 0 };
        ZSTD_inBuffer din   = { compressed, 0, 0 };
        ZSTD_outBuffer dout = { uncompressed, 0, 0 };

        cret = ZSTD_compressStream2(cctx, &cout, &cin, endOp);
        if (ZSTD_isError(cret))
            return cret;

        din.size = cout.pos;
        while (din.pos < din.size || (endOp == ZSTD_e_end && cret == 0)) {
            size_t dret;

            dout.pos = 0;
            dout.size = sizeof(uncompressed);
            dret = ZSTD_decompressStream(dctx, &dout, &din);
            if (ZSTD_isError(dret))
                return dret;
            XXH64_update(xxh, dout.dst, dout.pos);
            if (dret == 0)
                break;
        }
    } while (cin.pos < cin.size || (endOp != ZSTD_e_continue && cret != 0));
    return 0;
}

/* Generates some data and round trips it */
static size_t SEQ_generateRoundTrip(ZSTD_CCtx* cctx, ZSTD_DCtx* dctx,
                                    XXH64_state_t* xxh, SEQ_stream* seq,
                                    SEQ_gen_type type, unsigned value)
{
    static BYTE data[1024];
    size_t gen;

    do {
        SEQ_outBuffer sout = {data, sizeof(data), 0};
        size_t ret;
        gen = SEQ_gen(seq, type, value, &sout);

        ret = SEQ_roundTrip(cctx, dctx, xxh, sout.dst, sout.pos, ZSTD_e_continue);
        if (ZSTD_isError(ret))
            return ret;
    } while (gen != 0);

    return 0;
}

static size_t getCCtxParams(ZSTD_CCtx* zc, ZSTD_parameters* savedParams)
{
    int value;
    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_windowLog, (int*)&savedParams->cParams.windowLog));
    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_hashLog, (int*)&savedParams->cParams.hashLog));
    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_chainLog, (int*)&savedParams->cParams.chainLog));
    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_searchLog, (int*)&savedParams->cParams.searchLog));
    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_minMatch, (int*)&savedParams->cParams.minMatch));
    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_targetLength, (int*)&savedParams->cParams.targetLength));
    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_strategy, &value));
    savedParams->cParams.strategy = value;

    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_checksumFlag, &savedParams->fParams.checksumFlag));
    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_contentSizeFlag, &savedParams->fParams.contentSizeFlag));
    CHECK_RET_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_dictIDFlag, &value));
    savedParams->fParams.noDictIDFlag = !value;
    return 0;
}

static U32 badParameters(ZSTD_CCtx* zc, ZSTD_parameters const savedParams)
{
    ZSTD_parameters params;
    if (ZSTD_isError(getCCtxParams(zc, &params))) return 10;
    CHECK_RET(1, params.cParams.windowLog != savedParams.cParams.windowLog, "windowLog");
    CHECK_RET(2, params.cParams.hashLog != savedParams.cParams.hashLog, "hashLog");
    CHECK_RET(3, params.cParams.chainLog != savedParams.cParams.chainLog, "chainLog");
    CHECK_RET(4, params.cParams.searchLog != savedParams.cParams.searchLog, "searchLog");
    CHECK_RET(5, params.cParams.minMatch != savedParams.cParams.minMatch, "minMatch");
    CHECK_RET(6, params.cParams.targetLength != savedParams.cParams.targetLength, "targetLength");

    CHECK_RET(7, params.fParams.checksumFlag != savedParams.fParams.checksumFlag, "checksumFlag");
    CHECK_RET(8, params.fParams.contentSizeFlag != savedParams.fParams.contentSizeFlag, "contentSizeFlag");
    CHECK_RET(9, params.fParams.noDictIDFlag != savedParams.fParams.noDictIDFlag, "noDictIDFlag");
    return 0;
}

static int basicUnitTests(U32 seed, double compressibility, int bigTests)
{
    size_t const CNBufferSize = COMPRESSIBLE_NOISE_LENGTH;
    void* CNBuffer = malloc(CNBufferSize);
    size_t const skippableFrameSize = 200 KB;
    size_t const compressedBufferSize = (8 + skippableFrameSize) + ZSTD_compressBound(COMPRESSIBLE_NOISE_LENGTH);
    void* compressedBuffer = malloc(compressedBufferSize);
    size_t const decodedBufferSize = CNBufferSize;
    void* decodedBuffer = malloc(decodedBufferSize);
    size_t cSize;
    int testResult = 0;
    int testNb = 1;
    U32 coreSeed = 0;  /* this name to conform with CHECK_Z macro display */
    ZSTD_CStream* zc = ZSTD_createCStream();
    ZSTD_DStream* zd = ZSTD_createDStream();
    ZSTD_CCtx* mtctx = ZSTD_createCCtx();

    ZSTD_inBuffer  inBuff, inBuff2;
    ZSTD_outBuffer outBuff;
    buffer_t dictionary = kBuffNull;
    size_t const dictSize = 128 KB;
    unsigned dictID = 0;

    /* Create compressible test buffer */
    if (!CNBuffer || !compressedBuffer || !decodedBuffer || !zc || !zd || !mtctx) {
        DISPLAY("Not enough memory, aborting \n");
        goto _output_error;
    }
    RDG_genBuffer(CNBuffer, CNBufferSize, compressibility, 0., seed);

    CHECK_Z(ZSTD_CCtx_setParameter(mtctx, ZSTD_c_nbWorkers, 2));

    /* Create dictionary */
    DISPLAYLEVEL(3, "creating dictionary for unit tests \n");
    dictionary = FUZ_createDictionary(CNBuffer, CNBufferSize / 3, 16 KB, 48 KB);
    if (!dictionary.start) {
        DISPLAY("Error creating dictionary, aborting \n");
        goto _output_error;
    }
    dictID = ZDICT_getDictID(dictionary.start, dictionary.filled);

    /* Basic compression test */
    DISPLAYLEVEL(3, "test%3i : compress %u bytes : ", testNb++, COMPRESSIBLE_NOISE_LENGTH);
    CHECK_Z( ZSTD_initCStream(zc, 1 /* cLevel */) );
    outBuff.dst = (char*)(compressedBuffer);
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = CNBufferSize;
    inBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
    if (inBuff.pos != inBuff.size) goto _output_error;   /* entire input should be consumed */
    { size_t const r = ZSTD_endStream(zc, &outBuff);
      if (r != 0) goto _output_error; }  /* error, or some data not flushed */
    DISPLAYLEVEL(3, "OK (%u bytes)\n", (unsigned)outBuff.pos);

    /* generate skippable frame */
    MEM_writeLE32(compressedBuffer, ZSTD_MAGIC_SKIPPABLE_START);
    MEM_writeLE32(((char*)compressedBuffer)+4, (U32)skippableFrameSize);
    cSize = skippableFrameSize + 8;

    /* Basic compression test using dict */
    DISPLAYLEVEL(3, "test%3i : skipframe + compress %u bytes : ", testNb++, COMPRESSIBLE_NOISE_LENGTH);
    CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
    CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, 1) );
    CHECK_Z( ZSTD_CCtx_loadDictionary(zc, CNBuffer, dictSize) );
    outBuff.dst = (char*)(compressedBuffer)+cSize;
    assert(compressedBufferSize > cSize);
    outBuff.size = compressedBufferSize - cSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = CNBufferSize;
    inBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
    if (inBuff.pos != inBuff.size) goto _output_error;   /* entire input should be consumed */
    { size_t const r = ZSTD_endStream(zc, &outBuff);
      if (r != 0) goto _output_error; }  /* error, or some data not flushed */
    cSize += outBuff.pos;
    DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n",
                    (unsigned)cSize, (double)cSize/COMPRESSIBLE_NOISE_LENGTH*100);

    /* context size functions */
    DISPLAYLEVEL(3, "test%3i : estimate CStream size : ", testNb++);
    {   ZSTD_compressionParameters const cParams = ZSTD_getCParams(1, CNBufferSize, dictSize);
        size_t const cstreamSize = ZSTD_estimateCStreamSize_usingCParams(cParams);
        size_t const cdictSize = ZSTD_estimateCDictSize_advanced(dictSize, cParams, ZSTD_dlm_byCopy); /* uses ZSTD_initCStream_usingDict() */
        if (ZSTD_isError(cstreamSize)) goto _output_error;
        if (ZSTD_isError(cdictSize)) goto _output_error;
        DISPLAYLEVEL(3, "OK (%u bytes) \n", (unsigned)(cstreamSize + cdictSize));
    }

    /* context size functions */
    DISPLAYLEVEL(3, "test%3i : estimate CStream size using CCtxParams : ", testNb++);
    {   ZSTD_CCtx_params* const params = ZSTD_createCCtxParams();
        size_t cstreamSize, cctxSize;
        CHECK_Z( ZSTD_CCtxParams_setParameter(params, ZSTD_c_compressionLevel, 19) );
        cstreamSize = ZSTD_estimateCStreamSize_usingCCtxParams(params);
        CHECK_Z(cstreamSize);
        cctxSize = ZSTD_estimateCCtxSize_usingCCtxParams(params);
        CHECK_Z(cctxSize);
        if (cstreamSize <= cctxSize + 2 * ZSTD_BLOCKSIZE_MAX) goto _output_error;
        ZSTD_freeCCtxParams(params);
        DISPLAYLEVEL(3, "OK \n");
    }

    DISPLAYLEVEL(3, "test%3i : check actual CStream size : ", testNb++);
    {   size_t const s = ZSTD_sizeof_CStream(zc);
        if (ZSTD_isError(s)) goto _output_error;
        DISPLAYLEVEL(3, "OK (%u bytes) \n", (unsigned)s);
    }

    /* Attempt bad compression parameters */
    DISPLAYLEVEL(3, "test%3i : use bad compression parameters with ZSTD_initCStream_advanced : ", testNb++);
    {   size_t r;
        ZSTD_parameters params = ZSTD_getParams(1, 0, 0);
        params.cParams.minMatch = 2;
        r = ZSTD_initCStream_advanced(zc, NULL, 0, params, 0);
        if (!ZSTD_isError(r)) goto _output_error;
        DISPLAYLEVEL(3, "init error : %s \n", ZSTD_getErrorName(r));
    }

    /* skippable frame test */
    DISPLAYLEVEL(3, "test%3i : decompress skippable frame : ", testNb++);
    CHECK_Z( ZSTD_initDStream_usingDict(zd, CNBuffer, dictSize) );
    inBuff.src = compressedBuffer;
    inBuff.size = cSize;
    inBuff.pos = 0;
    outBuff.dst = decodedBuffer;
    outBuff.size = CNBufferSize;
    outBuff.pos = 0;
    {   size_t const r = ZSTD_decompressStream(zd, &outBuff, &inBuff);
        DISPLAYLEVEL(5, " ( ZSTD_decompressStream => %u ) ", (unsigned)r);
        if (r != 0) goto _output_error;
    }
    if (outBuff.pos != 0) goto _output_error;   /* skippable frame output len is 0 */
    DISPLAYLEVEL(3, "OK \n");

    /* Basic decompression test */
    inBuff2 = inBuff;
    DISPLAYLEVEL(3, "test%3i : decompress %u bytes : ", testNb++, COMPRESSIBLE_NOISE_LENGTH);
    ZSTD_initDStream_usingDict(zd, CNBuffer, dictSize);
    CHECK_Z( ZSTD_DCtx_setParameter(zd, ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_LIMIT_DEFAULT+1) );  /* large limit */
    { size_t const remaining = ZSTD_decompressStream(zd, &outBuff, &inBuff);
      if (remaining != 0) goto _output_error; }  /* should reach end of frame == 0; otherwise, some data left, or an error */
    if (outBuff.pos != CNBufferSize) goto _output_error;   /* should regenerate the same amount */
    if (inBuff.pos != inBuff.size) goto _output_error;   /* should have read the entire frame */
    DISPLAYLEVEL(3, "OK \n");

    /* Re-use without init */
    DISPLAYLEVEL(3, "test%3i : decompress again without init (re-use previous settings): ", testNb++);
    outBuff.pos = 0;
    { size_t const remaining = ZSTD_decompressStream(zd, &outBuff, &inBuff2);
      if (remaining != 0) goto _output_error; }  /* should reach end of frame == 0; otherwise, some data left, or an error */
    if (outBuff.pos != CNBufferSize) goto _output_error;   /* should regenerate the same amount */
    if (inBuff.pos != inBuff.size) goto _output_error;   /* should have read the entire frame */
    DISPLAYLEVEL(3, "OK \n");

    /* check regenerated data is byte exact */
    DISPLAYLEVEL(3, "test%3i : check decompressed result : ", testNb++);
    {   size_t i;
        for (i=0; i<CNBufferSize; i++) {
            if (((BYTE*)decodedBuffer)[i] != ((BYTE*)CNBuffer)[i]) goto _output_error;
    }   }
    DISPLAYLEVEL(3, "OK \n");

    /* check decompression fails early if first bytes are wrong */
    DISPLAYLEVEL(3, "test%3i : early decompression error if first bytes are incorrect : ", testNb++);
    {   const char buf[3] = { 0 };  /* too short, not enough to start decoding header */
        ZSTD_inBuffer inb = { buf, sizeof(buf), 0 };
        size_t const remaining = ZSTD_decompressStream(zd, &outBuff, &inb);
        if (!ZSTD_isError(remaining)) goto _output_error; /* should have errored out immediately (note: this does not test the exact error code) */
    }
    DISPLAYLEVEL(3, "OK \n");

    /* context size functions */
    DISPLAYLEVEL(3, "test%3i : estimate DStream size : ", testNb++);
    {   ZSTD_frameHeader fhi;
        const void* cStart = (char*)compressedBuffer + (skippableFrameSize + 8);
        size_t const gfhError = ZSTD_getFrameHeader(&fhi, cStart, cSize);
        if (gfhError!=0) goto _output_error;
        DISPLAYLEVEL(5, " (windowSize : %u) ", (unsigned)fhi.windowSize);
        {   size_t const s = ZSTD_estimateDStreamSize(fhi.windowSize)
                            /* uses ZSTD_initDStream_usingDict() */
                           + ZSTD_estimateDDictSize(dictSize, ZSTD_dlm_byCopy);
            if (ZSTD_isError(s)) goto _output_error;
            DISPLAYLEVEL(3, "OK (%u bytes) \n", (unsigned)s);
    }   }

    DISPLAYLEVEL(3, "test%3i : check actual DStream size : ", testNb++);
    { size_t const s = ZSTD_sizeof_DStream(zd);
      if (ZSTD_isError(s)) goto _output_error;
      DISPLAYLEVEL(3, "OK (%u bytes) \n", (unsigned)s);
    }

    /* Decompression by small increment */
    DISPLAYLEVEL(3, "test%3i : decompress byte-by-byte : ", testNb++);
    {   /* skippable frame */
        size_t r = 1;
        ZSTD_initDStream_usingDict(zd, CNBuffer, dictSize);
        inBuff.src = compressedBuffer;
        outBuff.dst = decodedBuffer;
        inBuff.pos = 0;
        outBuff.pos = 0;
        while (r) {   /* skippable frame */
            size_t const inSize = (FUZ_rand(&coreSeed) & 15) + 1;
            size_t const outSize = (FUZ_rand(&coreSeed) & 15) + 1;
            inBuff.size = inBuff.pos + inSize;
            outBuff.size = outBuff.pos + outSize;
            r = ZSTD_decompressStream(zd, &outBuff, &inBuff);
            if (ZSTD_isError(r)) DISPLAYLEVEL(4, "ZSTD_decompressStream on skippable frame error : %s \n", ZSTD_getErrorName(r));
            if (ZSTD_isError(r)) goto _output_error;
        }
        /* normal frame */
        ZSTD_initDStream_usingDict(zd, CNBuffer, dictSize);
        r=1;
        while (r) {
            size_t const inSize = FUZ_rand(&coreSeed) & 15;
            size_t const outSize = (FUZ_rand(&coreSeed) & 15) + (!inSize);   /* avoid having both sizes at 0 => would trigger a no_forward_progress error */
            inBuff.size = inBuff.pos + inSize;
            outBuff.size = outBuff.pos + outSize;
            r = ZSTD_decompressStream(zd, &outBuff, &inBuff);
            if (ZSTD_isError(r)) DISPLAYLEVEL(4, "ZSTD_decompressStream error : %s \n", ZSTD_getErrorName(r));
            if (ZSTD_isError(r)) goto _output_error;
        }
    }
    if (outBuff.pos != CNBufferSize) DISPLAYLEVEL(4, "outBuff.pos != CNBufferSize : should have regenerated same amount ! \n");
    if (outBuff.pos != CNBufferSize) goto _output_error;   /* should regenerate the same amount */
    if (inBuff.pos != cSize) DISPLAYLEVEL(4, "inBuff.pos != cSize : should have real all input ! \n");
    if (inBuff.pos != cSize) goto _output_error;   /* should have read the entire frame */
    DISPLAYLEVEL(3, "OK \n");

    /* check regenerated data is byte exact */
    DISPLAYLEVEL(3, "test%3i : check decompressed result : ", testNb++);
    {   size_t i;
        for (i=0; i<CNBufferSize; i++) {
            if (((BYTE*)decodedBuffer)[i] != ((BYTE*)CNBuffer)[i]) goto _output_error;
    }   }
    DISPLAYLEVEL(3, "OK \n");

    /* Decompression forward progress */
    DISPLAYLEVEL(3, "test%3i : generate error when ZSTD_decompressStream() doesn't progress : ", testNb++);
    {   /* skippable frame */
        size_t r = 0;
        int decNb = 0;
        int const maxDec = 100;
        inBuff.src = compressedBuffer;
        inBuff.size = cSize;
        inBuff.pos = 0;

        outBuff.dst = decodedBuffer;
        outBuff.pos = 0;
        outBuff.size = CNBufferSize-1;   /* 1 byte missing */

        for (decNb=0; decNb<maxDec; decNb++) {
            if (r==0) ZSTD_initDStream_usingDict(zd, CNBuffer, dictSize);
            r = ZSTD_decompressStream(zd, &outBuff, &inBuff);
            if (ZSTD_isError(r)) break;
        }
        if (!ZSTD_isError(r)) DISPLAYLEVEL(4, "ZSTD_decompressStream should have triggered a no_forward_progress error \n");
        if (!ZSTD_isError(r)) goto _output_error;   /* should have triggered no_forward_progress error */
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : NULL output and NULL input : ", testNb++);
    inBuff.src = NULL;
    inBuff.size = 0;
    inBuff.pos = 0;
    outBuff.dst = NULL;
    outBuff.size = 0;
    outBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
    CHECK(inBuff.pos != inBuff.size, "Entire input should be consumed");
    CHECK_Z( ZSTD_endStream(zc, &outBuff) );
    outBuff.dst = (char*)(compressedBuffer);
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    {   size_t const r = ZSTD_endStream(zc, &outBuff);
        CHECK(r != 0, "Error or some data not flushed (ret=%zu)", r);
    }
    inBuff.src = outBuff.dst;
    inBuff.size = outBuff.pos;
    inBuff.pos = 0;
    outBuff.dst = NULL;
    outBuff.size = 0;
    outBuff.pos = 0;
    CHECK_Z( ZSTD_initDStream(zd) );
    {   size_t const ret = ZSTD_decompressStream(zd, &outBuff, &inBuff);
        if (ret != 0) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK\n");

    DISPLAYLEVEL(3, "test%3i : NULL output buffer with non-NULL input : ", testNb++);
    {
        const char* test = "aa";
        inBuff.src = test;
        inBuff.size = 2;
        inBuff.pos = 0;
        outBuff.dst = NULL;
        outBuff.size = 0;
        outBuff.pos = 0;
        CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
        CHECK(inBuff.pos != inBuff.size, "Entire input should be consumed");
        CHECK_Z( ZSTD_endStream(zc, &outBuff) );
        outBuff.dst = (char*)(compressedBuffer);
        outBuff.size = compressedBufferSize;
        outBuff.pos = 0;
        {   size_t const r = ZSTD_endStream(zc, &outBuff);
            CHECK(r != 0, "Error or some data not flushed (ret=%zu)", r);
        }
        inBuff.src = outBuff.dst;
        inBuff.size = outBuff.pos;
        inBuff.pos = 0;
        outBuff.dst = NULL;
        outBuff.size = 0;
        outBuff.pos = 0;
        CHECK_Z( ZSTD_initDStream(zd) );
        CHECK_Z(ZSTD_decompressStream(zd, &outBuff, &inBuff));
    }

    DISPLAYLEVEL(3, "OK\n");
    /* _srcSize compression test */
    DISPLAYLEVEL(3, "test%3i : compress_srcSize %u bytes : ", testNb++, COMPRESSIBLE_NOISE_LENGTH);
    CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
    CHECK_Z( ZSTD_CCtx_refCDict(zc, NULL) );
    CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, 1) );
    CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, CNBufferSize) );
    outBuff.dst = (char*)(compressedBuffer);
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = CNBufferSize;
    inBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
    CHECK(inBuff.pos != inBuff.size, "Entire input should be consumed");
    {   size_t const r = ZSTD_endStream(zc, &outBuff);
        CHECK(r != 0, "Error or some data not flushed (ret=%zu)", r);
    }
    {   unsigned long long origSize = ZSTD_findDecompressedSize(outBuff.dst, outBuff.pos);
        CHECK(origSize == ZSTD_CONTENTSIZE_UNKNOWN, "Unknown!");
        CHECK((size_t)origSize != CNBufferSize, "Exact original size must be present (got %llu)", origSize);
    }
    DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/COMPRESSIBLE_NOISE_LENGTH*100);

    /* wrong _srcSize compression test */
    DISPLAYLEVEL(3, "test%3i : too large srcSize : %u bytes : ", testNb++, COMPRESSIBLE_NOISE_LENGTH-1);
    CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
    CHECK_Z( ZSTD_CCtx_refCDict(zc, NULL) );
    CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, 1) );
    CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, CNBufferSize+1) );
    outBuff.dst = (char*)(compressedBuffer);
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = CNBufferSize;
    inBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
    if (inBuff.pos != inBuff.size) goto _output_error;   /* entire input should be consumed */
    { size_t const r = ZSTD_endStream(zc, &outBuff);
      if (ZSTD_getErrorCode(r) != ZSTD_error_srcSize_wrong) goto _output_error;    /* must fail : wrong srcSize */
      DISPLAYLEVEL(3, "OK (error detected : %s) \n", ZSTD_getErrorName(r)); }

    /* wrong _srcSize compression test */
    DISPLAYLEVEL(3, "test%3i : too small srcSize : %u bytes : ", testNb++, COMPRESSIBLE_NOISE_LENGTH-1);
    CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
    CHECK_Z( ZSTD_CCtx_refCDict(zc, NULL) );
    CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, 1) );
    CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, CNBufferSize-1) );
    outBuff.dst = (char*)(compressedBuffer);
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = CNBufferSize;
    inBuff.pos = 0;
    {   size_t const r = ZSTD_compressStream(zc, &outBuff, &inBuff);
        if (ZSTD_getErrorCode(r) != ZSTD_error_srcSize_wrong) goto _output_error;    /* must fail : wrong srcSize */
        DISPLAYLEVEL(3, "OK (error detected : %s) \n", ZSTD_getErrorName(r));
    }

    DISPLAYLEVEL(3, "test%3i : wrong srcSize !contentSizeFlag : %u bytes : ", testNb++, COMPRESSIBLE_NOISE_LENGTH-1);
    {   CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
        CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_contentSizeFlag, 0) );
        CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, CNBufferSize - MIN(CNBufferSize, 200 KB)) );
        outBuff.dst = (char*)compressedBuffer;
        outBuff.size = compressedBufferSize;
        outBuff.pos = 0;
        inBuff.src = CNBuffer;
        inBuff.size = CNBufferSize;
        inBuff.pos = 0;
        {   size_t const r = ZSTD_compressStream(zc, &outBuff, &inBuff);
            if (ZSTD_getErrorCode(r) != ZSTD_error_srcSize_wrong) goto _output_error;    /* must fail : wrong srcSize */
            DISPLAYLEVEL(3, "OK (error detected : %s) \n", ZSTD_getErrorName(r));
    }   }

    /* Compression state re-use scenario */
    DISPLAYLEVEL(3, "test%3i : context re-use : ", testNb++);
    ZSTD_freeCStream(zc);
    zc = ZSTD_createCStream();
    if (zc==NULL) goto _output_error;   /* memory allocation issue */
    /* use 1 */
    {   size_t const inSize = 513;
        DISPLAYLEVEL(5, "use1 ");
        CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
        CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, 19) );
        CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, inSize) );
        inBuff.src = CNBuffer;
        inBuff.size = inSize;
        inBuff.pos = 0;
        outBuff.dst = (char*)(compressedBuffer)+cSize;
        outBuff.size = ZSTD_compressBound(inSize);
        outBuff.pos = 0;
        DISPLAYLEVEL(5, "compress1 ");
        CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
        if (inBuff.pos != inBuff.size) goto _output_error;   /* entire input should be consumed */
        DISPLAYLEVEL(5, "end1 ");
        if (ZSTD_endStream(zc, &outBuff) != 0) goto _output_error;  /* error, or some data not flushed */
    }
    /* use 2 */
    {   size_t const inSize = 1025;   /* will not continue, because tables auto-adjust and are therefore different size */
        DISPLAYLEVEL(5, "use2 ");
        CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
        CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, 19) );
        CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, inSize) );
        inBuff.src = CNBuffer;
        inBuff.size = inSize;
        inBuff.pos = 0;
        outBuff.dst = (char*)(compressedBuffer)+cSize;
        outBuff.size = ZSTD_compressBound(inSize);
        outBuff.pos = 0;
        DISPLAYLEVEL(5, "compress2 ");
        CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
        if (inBuff.pos != inBuff.size) goto _output_error;   /* entire input should be consumed */
        DISPLAYLEVEL(5, "end2 ");
        if (ZSTD_endStream(zc, &outBuff) != 0) goto _output_error;   /* error, or some data not flushed */
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Decompression single pass with empty frame */
    cSize = ZSTD_compress(compressedBuffer, compressedBufferSize, NULL, 0, 1);
    CHECK_Z(cSize);
    DISPLAYLEVEL(3, "test%3i : ZSTD_decompressStream() single pass on empty frame : ", testNb++);
    {   ZSTD_DCtx* dctx = ZSTD_createDCtx();
        size_t const dctxSize = ZSTD_sizeof_DCtx(dctx);
        CHECK_Z(ZSTD_DCtx_setParameter(dctx, ZSTD_d_stableOutBuffer, 1));

        outBuff.dst = decodedBuffer;
        outBuff.pos = 0;
        outBuff.size = CNBufferSize;

        inBuff.src = compressedBuffer;
        inBuff.size = cSize;
        inBuff.pos = 0;
        {   size_t const r = ZSTD_decompressStream(dctx, &outBuff, &inBuff);
            CHECK_Z(r);
            CHECK(r != 0, "Entire frame must be decompressed");
            CHECK(outBuff.pos != 0, "Wrong size!");
            CHECK(memcmp(CNBuffer, outBuff.dst, CNBufferSize) != 0, "Corruption!");
        }
        CHECK(dctxSize != ZSTD_sizeof_DCtx(dctx), "No buffers allocated");
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Decompression with ZSTD_d_stableOutBuffer */
    cSize = ZSTD_compress(compressedBuffer, compressedBufferSize, CNBuffer, CNBufferSize, 1);
    CHECK_Z(cSize);
    {   ZSTD_DCtx* dctx = ZSTD_createDCtx();
        size_t const dctxSize0 = ZSTD_sizeof_DCtx(dctx);
        size_t dctxSize1;
        CHECK_Z(ZSTD_DCtx_setParameter(dctx, ZSTD_d_stableOutBuffer, 1));

        outBuff.dst = decodedBuffer;
        outBuff.pos = 0;
        outBuff.size = CNBufferSize;

        DISPLAYLEVEL(3, "test%3i : ZSTD_decompressStream() single pass : ", testNb++);
        inBuff.src = compressedBuffer;
        inBuff.size = cSize;
        inBuff.pos = 0;
        {   size_t const r = ZSTD_decompressStream(dctx, &outBuff, &inBuff);
            CHECK_Z(r);
            CHECK(r != 0, "Entire frame must be decompressed");
            CHECK(outBuff.pos != CNBufferSize, "Wrong size!");
            CHECK(memcmp(CNBuffer, outBuff.dst, CNBufferSize) != 0, "Corruption!");
        }
        CHECK(dctxSize0 != ZSTD_sizeof_DCtx(dctx), "No buffers allocated");
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_decompressStream() stable out buffer : ", testNb++);
        outBuff.pos = 0;
        inBuff.pos = 0;
        inBuff.size = 0;
        while (inBuff.pos < cSize) {
            inBuff.size += MIN(cSize - inBuff.pos, 1 + (FUZ_rand(&coreSeed) & 15));
            CHECK_Z(ZSTD_decompressStream(dctx, &outBuff, &inBuff));
        }
        CHECK(outBuff.pos != CNBufferSize, "Wrong size!");
        CHECK(memcmp(CNBuffer, outBuff.dst, CNBufferSize) != 0, "Corruption!");
        dctxSize1 = ZSTD_sizeof_DCtx(dctx);
        CHECK(!(dctxSize0 < dctxSize1), "Input buffer allocated");
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_decompressStream() stable out buffer too small : ", testNb++);
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
        CHECK_Z(ZSTD_DCtx_setParameter(dctx, ZSTD_d_stableOutBuffer, 1));
        inBuff.src = compressedBuffer;
        inBuff.size = cSize;
        inBuff.pos = 0;
        outBuff.pos = 0;
        outBuff.size = CNBufferSize - 1;
        {   size_t const r = ZSTD_decompressStream(dctx, &outBuff, &inBuff);
            CHECK(ZSTD_getErrorCode(r) != ZSTD_error_dstSize_tooSmall, "Must error but got %s", ZSTD_getErrorName(r));
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_decompressStream() stable out buffer modified : ", testNb++);
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
        CHECK_Z(ZSTD_DCtx_setParameter(dctx, ZSTD_d_stableOutBuffer, 1));
        inBuff.src = compressedBuffer;
        inBuff.size = cSize - 1;
        inBuff.pos = 0;
        outBuff.pos = 0;
        outBuff.size = CNBufferSize;
        CHECK_Z(ZSTD_decompressStream(dctx, &outBuff, &inBuff));
        ++inBuff.size;
        outBuff.pos = 0;
        {   size_t const r = ZSTD_decompressStream(dctx, &outBuff, &inBuff);
            CHECK(ZSTD_getErrorCode(r) != ZSTD_error_dstBuffer_wrong, "Must error but got %s", ZSTD_getErrorName(r));
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_decompressStream() buffered output : ", testNb++);
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
        CHECK_Z(ZSTD_DCtx_setParameter(dctx, ZSTD_d_stableOutBuffer, 0));
        outBuff.pos = 0;
        inBuff.pos = 0;
        inBuff.size = 0;
        while (inBuff.pos < cSize) {
            inBuff.size += MIN(cSize - inBuff.pos, 1 + (FUZ_rand(&coreSeed) & 15));
            CHECK_Z(ZSTD_decompressStream(dctx, &outBuff, &inBuff));
        }
        CHECK(outBuff.pos != CNBufferSize, "Wrong size!");
        CHECK(memcmp(CNBuffer, outBuff.dst, CNBufferSize) != 0, "Corruption!");
        CHECK(!(dctxSize1 < ZSTD_sizeof_DCtx(dctx)), "Output buffer allocated");
        DISPLAYLEVEL(3, "OK \n");

        ZSTD_freeDCtx(dctx);
    }

    /* Compression with ZSTD_c_stable{In,Out}Buffer */
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_inBuffer in;
        ZSTD_outBuffer out;
        size_t cctxSize1;
        size_t cctxSize2;
        assert(cctx != NULL);
        in.src = CNBuffer;
        in.size = CNBufferSize;
        out.dst = compressedBuffer;
        out.size = compressedBufferSize;
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
        DISPLAYLEVEL(3, "test%3i : ZSTD_compress2() uses stable input and output : ", testNb++);
        CHECK_Z(cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBufferSize));
        CHECK(!(cSize < ZSTD_compressBound(CNBufferSize)), "cSize too large for test");
        CHECK_Z(cSize = ZSTD_compress2(cctx, compressedBuffer, cSize + 4, CNBuffer, CNBufferSize));
        CHECK_Z(cctxSize1 = ZSTD_sizeof_CCtx(cctx));
        /* @cctxSize2 : sizeof_CCtx when doing full streaming (no stable in/out) */
        {   ZSTD_CCtx* const cctx2 = ZSTD_createCCtx();
            assert(cctx2 != NULL);
            in.pos = out.pos = 0;
            CHECK_Z(ZSTD_compressStream2(cctx2, &out, &in, ZSTD_e_continue));
            CHECK(!(ZSTD_compressStream2(cctx2, &out, &in, ZSTD_e_end) == 0), "Not finished");
            CHECK_Z(cctxSize2 = ZSTD_sizeof_CCtx(cctx2));
            ZSTD_freeCCtx(cctx2);
        }
        /* @cctxSize1 : sizeof_CCtx when doing single-shot compression (no streaming) */
        {   ZSTD_CCtx* const cctx1 = ZSTD_createCCtx();
            ZSTD_parameters params = ZSTD_getParams(0, CNBufferSize, 0);
            size_t cSize3;
            assert(cctx1 != NULL);
            params.fParams.checksumFlag = 1;
            cSize3 = ZSTD_compress_advanced(cctx1, compressedBuffer, compressedBufferSize, CNBuffer, CNBufferSize, NULL, 0, params);
            CHECK_Z(cSize3);
            CHECK(!(cSize == cSize3), "Must be same compressed size");
            CHECK(!(cctxSize1 == ZSTD_sizeof_CCtx(cctx1)), "Must be same CCtx size");
            ZSTD_freeCCtx(cctx1);
        }
        CHECK(!(cctxSize1 < cctxSize2), "Stable buffers means less allocated size");
        CHECK_Z(ZSTD_decompress(decodedBuffer, CNBufferSize, compressedBuffer, cSize));
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compress2() doesn't modify user parameters : ", testNb++);
        {   int stableInBuffer;
            int stableOutBuffer;
            CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_stableInBuffer, &stableInBuffer));
            CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_stableOutBuffer, &stableOutBuffer));
            CHECK(!(stableInBuffer == 0), "Modified");
            CHECK(!(stableOutBuffer == 0), "Modified");
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_stableInBuffer, 1));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_stableOutBuffer, 1));
            CHECK_Z(cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBufferSize));
            CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_stableInBuffer, &stableInBuffer));
            CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_stableOutBuffer, &stableOutBuffer));
            CHECK(!(stableInBuffer == 1), "Modified");
            CHECK(!(stableOutBuffer == 1), "Modified");
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compressStream2() with ZSTD_c_stableInBuffer and ZSTD_c_stableOutBuffer : ", testNb++);
        CHECK_Z(ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_stableInBuffer, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_stableOutBuffer, 1));
        in.pos = out.pos = 0;
        CHECK(!(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end) == 0), "Not finished");
        CHECK_Z(ZSTD_decompress(decodedBuffer, CNBufferSize, compressedBuffer, cSize));
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compressStream2() ZSTD_c_stableInBuffer and ZSTD_c_stableOutBuffer allocated size : ", testNb++);
        {   size_t const cctxSize = ZSTD_sizeof_CCtx(cctx);
            CHECK(!(cctxSize1 == cctxSize), "Must be the same size as single pass");
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compressStream2() with ZSTD_c_stableInBuffer only : ", testNb++);
        CHECK_Z(ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_stableInBuffer, 1));
        in.pos = out.pos = 0;
        out.size = cSize / 4;
        for (;;) {
            size_t const ret = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
            CHECK_Z(ret);
            if (ret == 0)
                break;
            out.size = MIN(out.size + cSize / 4, compressedBufferSize);
        }
        CHECK_Z(ZSTD_decompress(decodedBuffer, CNBufferSize, compressedBuffer, cSize));
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compressStream2() ZSTD_c_stableInBuffer modify buffer : ", testNb++);
        in.pos = out.pos = 0;
        out.size = cSize / 4;
        CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));
        in.src = (char const*)in.src + in.pos;
        in.size -= in.pos;
        in.pos = 0;
        {   size_t const ret = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
            CHECK(!ZSTD_isError(ret), "Must error");
            CHECK(!(ZSTD_getErrorCode(ret) == ZSTD_error_stabilityCondition_notRespected), "Must be this error");
        }
        DISPLAYLEVEL(3, "OK \n");

        /* stableSrc + streaming */
        DISPLAYLEVEL(3, "test%3i : ZSTD_c_stableInBuffer compatibility with compressStream, flushStream and endStream : ", testNb++);
        CHECK_Z( ZSTD_initCStream(cctx, 1) );
        CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_stableInBuffer, 1) );
        {   ZSTD_inBuffer inBuf;
            ZSTD_outBuffer outBuf;
            const size_t nonZeroStartPos = 18;
            const size_t inputSize = 500;
            inBuf.src = CNBuffer;
            inBuf.size = 100;
            inBuf.pos = nonZeroStartPos;
            outBuf.dst = (char*)(compressedBuffer)+cSize;
            outBuf.size = ZSTD_compressBound(inputSize);
            outBuf.pos = 0;
            CHECK_Z( ZSTD_compressStream(cctx, &outBuf, &inBuf) );
            inBuf.size = 200;
            CHECK_Z( ZSTD_compressStream(cctx, &outBuf, &inBuf) );
            CHECK_Z( ZSTD_flushStream(cctx, &outBuf) );
            inBuf.size = nonZeroStartPos + inputSize;
            CHECK_Z( ZSTD_compressStream(cctx, &outBuf, &inBuf) );
            CHECK(ZSTD_endStream(cctx, &outBuf) != 0, "compression should be successful and fully flushed");
            {   const void* const realSrcStart = (const char*)inBuf.src + nonZeroStartPos;
                void* const verifBuf = (char*)outBuf.dst + outBuf.pos;
                const size_t decSize = ZSTD_decompress(verifBuf, inputSize, outBuf.dst, outBuf.pos);
                CHECK_Z(decSize);
                CHECK(decSize != inputSize, "regenerated %zu bytes, instead of %zu", decSize, inputSize);
                CHECK(memcmp(realSrcStart, verifBuf, inputSize) != 0, "regenerated data different from original");
        }   }
        DISPLAYLEVEL(3, "OK \n");

        /* stableSrc + streaming */
        DISPLAYLEVEL(3, "test%3i : ZSTD_c_stableInBuffer compatibility with compressStream2, using different end directives : ", testNb++);
        CHECK_Z( ZSTD_initCStream(cctx, 1) );
        CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_stableInBuffer, 1) );
        {   ZSTD_inBuffer inBuf;
            ZSTD_outBuffer outBuf;
            const size_t nonZeroStartPos = 18;
            const size_t inputSize = 500;
            inBuf.src = CNBuffer;
            inBuf.size = 100;
            inBuf.pos = nonZeroStartPos;
            outBuf.dst = (char*)(compressedBuffer)+cSize;
            outBuf.size = ZSTD_compressBound(inputSize);
            outBuf.pos = 0;
            CHECK_Z( ZSTD_compressStream2(cctx, &outBuf, &inBuf, ZSTD_e_continue) );
            inBuf.size = 200;
            CHECK_Z( ZSTD_compressStream2(cctx, &outBuf, &inBuf, ZSTD_e_continue) );
            CHECK_Z( ZSTD_compressStream2(cctx, &outBuf, &inBuf, ZSTD_e_flush) );
            inBuf.size = nonZeroStartPos + inputSize;
            CHECK_Z( ZSTD_compressStream2(cctx, &outBuf, &inBuf, ZSTD_e_continue) );
            CHECK( ZSTD_compressStream2(cctx, &outBuf, &inBuf, ZSTD_e_end) != 0, "compression should be successful and fully flushed");
            {   const void* const realSrcStart = (const char*)inBuf.src + nonZeroStartPos;
                void* const verifBuf = (char*)outBuf.dst + outBuf.pos;
                const size_t decSize = ZSTD_decompress(verifBuf, inputSize, outBuf.dst, outBuf.pos);
                CHECK_Z(decSize);
                CHECK(decSize != inputSize, "regenerated %zu bytes, instead of %zu", decSize, inputSize);
                CHECK(memcmp(realSrcStart, verifBuf, inputSize) != 0, "regenerated data different from original");
        }   }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compressStream2() with ZSTD_c_stableInBuffer: context size : ", testNb++);
        {   size_t const cctxSize = ZSTD_sizeof_CCtx(cctx);
            DISPLAYLEVEL(4, "cctxSize1=%zu; cctxSize=%zu; cctxSize2=%zu : ", cctxSize1, cctxSize, cctxSize2);
            CHECK(!(cctxSize1 < cctxSize), "Must be bigger than single-pass");
            CHECK(!(cctxSize < cctxSize2), "Must be smaller than streaming");
            cctxSize1 = cctxSize;
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compressStream2() with ZSTD_c_stableOutBuffer only : ", testNb++);
        CHECK_Z(ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_stableOutBuffer, 1));
        in.src = CNBuffer;
        in.pos = out.pos = 0;
        in.size = MIN(CNBufferSize, 10);
        out.size = compressedBufferSize;
        CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush));
        in.pos = 0;
        in.size = CNBufferSize - in.size;
        CHECK(!(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end) == 0), "Not finished");
        CHECK_Z(ZSTD_decompress(decodedBuffer, CNBufferSize, compressedBuffer, out.pos));
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compressStream2() ZSTD_c_stableOutBuffer modify buffer : ", testNb++);
        in.pos = out.pos = 0;
        in.size = CNBufferSize;
        CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue));
        in.pos = out.pos = 0;
        {   size_t const ret = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue);
            CHECK(!ZSTD_isError(ret), "Must have errored");
            CHECK(!(ZSTD_getErrorCode(ret) == ZSTD_error_stabilityCondition_notRespected), "Must be this error");
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compressStream2() with ZSTD_c_stableOutBuffer: context size : ", testNb++);
        {   size_t const cctxSize = ZSTD_sizeof_CCtx(cctx);
            DISPLAYLEVEL(4, "cctxSize1=%zu; cctxSize=%zu; cctxSize2=%zu : ", cctxSize1, cctxSize, cctxSize2);
            CHECK(!(cctxSize1 < cctxSize), "Must be bigger than single-pass and stableInBuffer");
            CHECK(!(cctxSize < cctxSize2), "Must be smaller than streaming");
        }
        DISPLAYLEVEL(3, "OK \n");

        ZSTD_freeCCtx(cctx);
    }

    /* CDict scenario */
    DISPLAYLEVEL(3, "test%3i : digested dictionary : ", testNb++);
    {   ZSTD_CDict* const cdict = ZSTD_createCDict(dictionary.start, dictionary.filled, 1 /*byRef*/ );
        size_t const initError = ZSTD_initCStream_usingCDict(zc, cdict);
        DISPLAYLEVEL(5, "ZSTD_initCStream_usingCDict result : %u ", (unsigned)initError);
        if (ZSTD_isError(initError)) goto _output_error;
        outBuff.dst = compressedBuffer;
        outBuff.size = compressedBufferSize;
        outBuff.pos = 0;
        inBuff.src = CNBuffer;
        inBuff.size = CNBufferSize;
        inBuff.pos = 0;
        DISPLAYLEVEL(5, "- starting ZSTD_compressStream ");
        CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
        if (inBuff.pos != inBuff.size) goto _output_error;   /* entire input should be consumed */
        {   size_t const r = ZSTD_endStream(zc, &outBuff);
            DISPLAYLEVEL(5, "- ZSTD_endStream result : %u ", (unsigned)r);
            if (r != 0) goto _output_error;  /* error, or some data not flushed */
        }
        cSize = outBuff.pos;
        ZSTD_freeCDict(cdict);
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBufferSize*100);
    }

    DISPLAYLEVEL(3, "test%3i : check CStream size : ", testNb++);
    { size_t const s = ZSTD_sizeof_CStream(zc);
      if (ZSTD_isError(s)) goto _output_error;
      DISPLAYLEVEL(3, "OK (%u bytes) \n", (unsigned)s);
    }

    DISPLAYLEVEL(4, "test%3i : check Dictionary ID : ", testNb++);
    { unsigned const dID = ZSTD_getDictID_fromFrame(compressedBuffer, cSize);
      if (dID != dictID) goto _output_error;
      DISPLAYLEVEL(4, "OK (%u) \n", dID);
    }

    /* DDict scenario */
    DISPLAYLEVEL(3, "test%3i : decompress %u bytes with digested dictionary : ", testNb++, (unsigned)CNBufferSize);
    {   ZSTD_DDict* const ddict = ZSTD_createDDict(dictionary.start, dictionary.filled);
        size_t const initError = ZSTD_initDStream_usingDDict(zd, ddict);
        if (ZSTD_isError(initError)) goto _output_error;
        outBuff.dst = decodedBuffer;
        outBuff.size = CNBufferSize;
        outBuff.pos = 0;
        inBuff.src = compressedBuffer;
        inBuff.size = cSize;
        inBuff.pos = 0;
        { size_t const r = ZSTD_decompressStream(zd, &outBuff, &inBuff);
          if (r != 0) goto _output_error; }  /* should reach end of frame == 0; otherwise, some data left, or an error */
        if (outBuff.pos != CNBufferSize) goto _output_error;   /* should regenerate the same amount */
        if (inBuff.pos != inBuff.size) goto _output_error;   /* should have read the entire frame */
        ZSTD_freeDDict(ddict);
        DISPLAYLEVEL(3, "OK \n");
    }

    /* Memory restriction */
    DISPLAYLEVEL(3, "test%3i : maxWindowSize < frame requirement : ", testNb++);
    ZSTD_initDStream_usingDict(zd, CNBuffer, dictSize);
    CHECK_Z( ZSTD_DCtx_setParameter(zd, ZSTD_d_windowLogMax, 10) );  /* too small limit */
    outBuff.dst = decodedBuffer;
    outBuff.size = CNBufferSize;
    outBuff.pos = 0;
    inBuff.src = compressedBuffer;
    inBuff.size = cSize;
    inBuff.pos = 0;
    { size_t const r = ZSTD_decompressStream(zd, &outBuff, &inBuff);
      if (!ZSTD_isError(r)) goto _output_error;  /* must fail : frame requires > 100 bytes */
      DISPLAYLEVEL(3, "OK (%s)\n", ZSTD_getErrorName(r)); }
    ZSTD_DCtx_reset(zd, ZSTD_reset_session_and_parameters);   /* leave zd in good shape for next tests */

    DISPLAYLEVEL(3, "test%3i : dictionary source size and level : ", testNb++);
    {   ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        int const maxLevel = 16;   /* first level with zstd_opt */
        int level;
        assert(maxLevel < ZSTD_maxCLevel());
        CHECK_Z( ZSTD_DCtx_loadDictionary_byReference(dctx, dictionary.start, dictionary.filled) );
        for (level = 1; level <= maxLevel; ++level) {
            ZSTD_CDict* const cdict = ZSTD_createCDict(dictionary.start, dictionary.filled, level);
            size_t const maxSize = MIN(1 MB, CNBufferSize);
            size_t size;
            for (size = 512; size <= maxSize; size <<= 1) {
                U64 const crcOrig = XXH64(CNBuffer, size, 0);
                ZSTD_CCtx* const cctx = ZSTD_createCCtx();
                ZSTD_parameters savedParams;
                getCCtxParams(cctx, &savedParams);
                outBuff.dst = compressedBuffer;
                outBuff.size = compressedBufferSize;
                outBuff.pos = 0;
                inBuff.src = CNBuffer;
                inBuff.size = size;
                inBuff.pos = 0;
                CHECK_Z(ZSTD_CCtx_refCDict(cctx, cdict));
                CHECK_Z(ZSTD_compressStream2(cctx, &outBuff, &inBuff, ZSTD_e_end));
                CHECK(badParameters(cctx, savedParams), "Bad CCtx params");
                if (inBuff.pos != inBuff.size) goto _output_error;
                {   ZSTD_outBuffer decOut = {decodedBuffer, size, 0};
                    ZSTD_inBuffer decIn = {outBuff.dst, outBuff.pos, 0};
                    CHECK_Z( ZSTD_decompressStream(dctx, &decOut, &decIn) );
                    if (decIn.pos != decIn.size) goto _output_error;
                    if (decOut.pos != size) goto _output_error;
                    {   U64 const crcDec = XXH64(decOut.dst, decOut.pos, 0);
                        if (crcDec != crcOrig) goto _output_error;
                }   }
                ZSTD_freeCCtx(cctx);
            }
            ZSTD_freeCDict(cdict);
        }
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK\n");

    ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters);
    CHECK_Z( ZSTD_CCtx_loadDictionary(zc, dictionary.start, dictionary.filled) );
    cSize = ZSTD_compress2(zc, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBufferSize, 100 KB));
    CHECK_Z(cSize);
    DISPLAYLEVEL(3, "test%3i : ZSTD_decompressStream() with dictionary : ", testNb++);
    {
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        /* We should fail to decompress without a dictionary. */
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            size_t const ret = ZSTD_decompressStream(dctx, &out, &in);
            if (!ZSTD_isError(ret)) goto _output_error;
        }
        /* We should succeed to decompress with the dictionary. */
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
        CHECK_Z( ZSTD_DCtx_loadDictionary(dctx, dictionary.start, dictionary.filled) );
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            if (ZSTD_decompressStream(dctx, &out, &in) != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
        }
        /* The dictionary should persist across calls. */
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            if (ZSTD_decompressStream(dctx, &out, &in) != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
        }
        /* The dictionary should not be cleared by ZSTD_reset_session_only. */
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            if (ZSTD_decompressStream(dctx, &out, &in) != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
        }
        /* When we reset the context the dictionary is cleared. */
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            size_t const ret = ZSTD_decompressStream(dctx, &out, &in);
            if (!ZSTD_isError(ret)) goto _output_error;
        }
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_resetDStream() with dictionary : ", testNb++);
    {
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        /* We should succeed to decompress with the dictionary. */
        ZSTD_resetDStream(dctx);
        CHECK_Z( ZSTD_DCtx_loadDictionary(dctx, dictionary.start, dictionary.filled) );
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            if (ZSTD_decompressStream(dctx, &out, &in) != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
        }
        /* The dictionary should not be cleared by ZSTD_resetDStream(). */
        ZSTD_resetDStream(dctx);
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            if (ZSTD_decompressStream(dctx, &out, &in) != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
        }
        /* The dictionary should be cleared by ZSTD_initDStream(). */
        CHECK_Z( ZSTD_initDStream(dctx) );
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            size_t const ret = ZSTD_decompressStream(dctx, &out, &in);
            if (!ZSTD_isError(ret)) goto _output_error;
        }
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_decompressStream() with ddict : ", testNb++);
    {
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        ZSTD_DDict* ddict = ZSTD_createDDict(dictionary.start, dictionary.filled);
        /* We should succeed to decompress with the ddict. */
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
        CHECK_Z( ZSTD_DCtx_refDDict(dctx, ddict) );
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            if (ZSTD_decompressStream(dctx, &out, &in) != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
        }
        /* The ddict should persist across calls. */
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            if (ZSTD_decompressStream(dctx, &out, &in) != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
        }
        /* When we reset the context the ddict is cleared. */
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            size_t const ret = ZSTD_decompressStream(dctx, &out, &in);
            if (!ZSTD_isError(ret)) goto _output_error;
        }
        ZSTD_freeDCtx(dctx);
        ZSTD_freeDDict(ddict);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_decompressDCtx() with prefix : ", testNb++);
    {
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        /* We should succeed to decompress with the prefix. */
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
        CHECK_Z( ZSTD_DCtx_refPrefix_advanced(dctx, dictionary.start, dictionary.filled, ZSTD_dct_auto) );
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            if (ZSTD_decompressStream(dctx, &out, &in) != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
        }
        /* The prefix should be cleared after the first compression. */
        {   ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            size_t const ret = ZSTD_decompressStream(dctx, &out, &in);
            if (!ZSTD_isError(ret)) goto _output_error;
        }
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_initDStream*() with dictionary : ", testNb++);
    {
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        ZSTD_DDict* ddict = ZSTD_createDDict(dictionary.start, dictionary.filled);
        size_t ret;
        /* We should succeed to decompress with the dictionary. */
        CHECK_Z( ZSTD_initDStream_usingDict(dctx, dictionary.start, dictionary.filled) );
        CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, decodedBufferSize, compressedBuffer, cSize) );
        /* The dictionary should persist across calls. */
        CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, decodedBufferSize, compressedBuffer, cSize) );
        /* We should succeed to decompress with the ddict. */
        CHECK_Z( ZSTD_initDStream_usingDDict(dctx, ddict) );
        CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, decodedBufferSize, compressedBuffer, cSize) );
        /* The ddict should persist across calls. */
        CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, decodedBufferSize, compressedBuffer, cSize) );
        /* When we reset the context the ddict is cleared. */
        CHECK_Z( ZSTD_initDStream(dctx) );
        ret = ZSTD_decompressDCtx(dctx, decodedBuffer, decodedBufferSize, compressedBuffer, cSize);
        if (!ZSTD_isError(ret)) goto _output_error;
        ZSTD_freeDCtx(dctx);
        ZSTD_freeDDict(ddict);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_initCStream_usingCDict_advanced with masked dictID : ", testNb++);
    {   ZSTD_compressionParameters const cParams = ZSTD_getCParams(1, CNBufferSize, dictionary.filled);
        ZSTD_frameParameters const fParams = { 1 /* contentSize */, 1 /* checksum */, 1 /* noDictID */};
        ZSTD_CDict* const cdict = ZSTD_createCDict_advanced(dictionary.start, dictionary.filled, ZSTD_dlm_byRef, ZSTD_dct_auto, cParams, ZSTD_defaultCMem);
        size_t const initError = ZSTD_initCStream_usingCDict_advanced(zc, cdict, fParams, CNBufferSize);
        if (ZSTD_isError(initError)) goto _output_error;
        outBuff.dst = compressedBuffer;
        outBuff.size = compressedBufferSize;
        outBuff.pos = 0;
        inBuff.src = CNBuffer;
        inBuff.size = CNBufferSize;
        inBuff.pos = 0;
        CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
        if (inBuff.pos != inBuff.size) goto _output_error;  /* entire input should be consumed */
        { size_t const r = ZSTD_endStream(zc, &outBuff);
          if (r != 0) goto _output_error; }  /* error, or some data not flushed */
        cSize = outBuff.pos;
        ZSTD_freeCDict(cdict);
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBufferSize*100);
    }

    DISPLAYLEVEL(3, "test%3i : try retrieving dictID from frame : ", testNb++);
    {   U32 const did = ZSTD_getDictID_fromFrame(compressedBuffer, cSize);
        if (did != 0) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK (not detected) \n");

    DISPLAYLEVEL(3, "test%3i : decompress without dictionary : ", testNb++);
    {   size_t const r = ZSTD_decompress(decodedBuffer, CNBufferSize, compressedBuffer, cSize);
        if (!ZSTD_isError(r)) goto _output_error;  /* must fail : dictionary not used */
        DISPLAYLEVEL(3, "OK (%s)\n", ZSTD_getErrorName(r));
    }

    DISPLAYLEVEL(3, "test%3i : compress with ZSTD_CCtx_refPrefix : ", testNb++);
    CHECK_Z( ZSTD_CCtx_refPrefix(zc, dictionary.start, dictionary.filled) );
    outBuff.dst = compressedBuffer;
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = CNBufferSize;
    inBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream2(zc, &outBuff, &inBuff, ZSTD_e_end) );
    if (inBuff.pos != inBuff.size) goto _output_error;  /* entire input should be consumed */
    cSize = outBuff.pos;
    DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBufferSize*100);

    DISPLAYLEVEL(3, "test%3i : decompress with ZSTD_DCtx_refPrefix : ", testNb++);
    CHECK_Z( ZSTD_DCtx_refPrefix(zd, dictionary.start, dictionary.filled) );
    outBuff.dst = decodedBuffer;
    outBuff.size = CNBufferSize;
    outBuff.pos = 0;
    inBuff.src = compressedBuffer;
    inBuff.size = cSize;
    inBuff.pos = 0;
    CHECK_Z( ZSTD_decompressStream(zd, &outBuff, &inBuff) );
    if (inBuff.pos != inBuff.size) goto _output_error;  /* entire input should be consumed */
    if (outBuff.pos != CNBufferSize) goto _output_error;  /* must regenerate whole input */
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress without dictionary (should fail): ", testNb++);
    {   size_t const r = ZSTD_decompress(decodedBuffer, CNBufferSize, compressedBuffer, cSize);
        if (!ZSTD_isError(r)) goto _output_error;  /* must fail : dictionary not used */
        DISPLAYLEVEL(3, "OK (%s)\n", ZSTD_getErrorName(r));
    }

    DISPLAYLEVEL(3, "test%3i : compress again with ZSTD_compressStream2 : ", testNb++);
    outBuff.dst = compressedBuffer;
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = CNBufferSize;
    inBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream2(zc, &outBuff, &inBuff, ZSTD_e_end) );
    if (inBuff.pos != inBuff.size) goto _output_error;  /* entire input should be consumed */
    cSize = outBuff.pos;
    DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBufferSize*100);

    DISPLAYLEVEL(3, "test%3i : decompress without dictionary (should work): ", testNb++);
    CHECK_Z( ZSTD_decompress(decodedBuffer, CNBufferSize, compressedBuffer, cSize) );
    DISPLAYLEVEL(3, "OK \n");

    /* Empty srcSize */
    DISPLAYLEVEL(3, "test%3i : ZSTD_initCStream_advanced with pledgedSrcSize=0 and dict : ", testNb++);
    {   ZSTD_parameters params = ZSTD_getParams(5, 0, 0);
        params.fParams.contentSizeFlag = 1;
        CHECK_Z( ZSTD_initCStream_advanced(zc, dictionary.start, dictionary.filled, params, 0 /* pledgedSrcSize==0 means "empty" when params.fParams.contentSizeFlag is set */) );
    } /* cstream advanced shall write content size = 0 */
    outBuff.dst = compressedBuffer;
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = 0;
    inBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
    if (ZSTD_endStream(zc, &outBuff) != 0) goto _output_error;
    cSize = outBuff.pos;
    if (ZSTD_findDecompressedSize(compressedBuffer, cSize) != 0) goto _output_error;
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : pledgedSrcSize == 0 behaves properly with ZSTD_initCStream_advanced : ", testNb++);
    {   ZSTD_parameters params = ZSTD_getParams(5, 0, 0);
        params.fParams.contentSizeFlag = 1;
        CHECK_Z( ZSTD_initCStream_advanced(zc, NULL, 0, params, 0) );
    } /* cstream advanced shall write content size = 0 */
    inBuff.src = CNBuffer;
    inBuff.size = 0;
    inBuff.pos = 0;
    outBuff.dst = compressedBuffer;
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
    if (ZSTD_endStream(zc, &outBuff) != 0) goto _output_error;
    cSize = outBuff.pos;
    if (ZSTD_findDecompressedSize(compressedBuffer, cSize) != 0) goto _output_error;

    CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
    CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, ZSTD_CONTENTSIZE_UNKNOWN) );
    outBuff.dst = compressedBuffer;
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = 0;
    inBuff.pos = 0;
    CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );
    if (ZSTD_endStream(zc, &outBuff) != 0) goto _output_error;
    cSize = outBuff.pos;
    if (ZSTD_findDecompressedSize(compressedBuffer, cSize) != ZSTD_CONTENTSIZE_UNKNOWN) goto _output_error;
    DISPLAYLEVEL(3, "OK \n");

    /* Basic multithreading compression test */
    DISPLAYLEVEL(3, "test%3i : compress %u bytes with multiple threads : ", testNb++, COMPRESSIBLE_NOISE_LENGTH);
    {   int jobSize;
        CHECK_Z( ZSTD_CCtx_getParameter(mtctx, ZSTD_c_jobSize, &jobSize));
        CHECK(jobSize != 0, "job size non-zero");
        CHECK_Z( ZSTD_CCtx_getParameter(mtctx, ZSTD_c_jobSize, &jobSize));
        CHECK(jobSize != 0, "job size non-zero");
    }
    outBuff.dst = compressedBuffer;
    outBuff.size = compressedBufferSize;
    outBuff.pos = 0;
    inBuff.src = CNBuffer;
    inBuff.size = CNBufferSize;
    inBuff.pos = 0;
    {   size_t const compressResult = ZSTD_compressStream2(mtctx, &outBuff, &inBuff, ZSTD_e_end);
        if (compressResult != 0) goto _output_error;  /* compression must be completed in a single round */
    }
    if (inBuff.pos != inBuff.size) goto _output_error;   /* entire input should be consumed */
    {   size_t const compressedSize = ZSTD_findFrameCompressedSize(compressedBuffer, outBuff.pos);
        if (compressedSize != outBuff.pos) goto _output_error;  /* must be a full valid frame */
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Complex multithreading + dictionary test */
    {   U32 const nbWorkers = 2;
        size_t const jobSize = 4 * 1 MB;
        size_t const srcSize = jobSize * nbWorkers;  /* we want each job to have predictable size */
        size_t const segLength = 2 KB;
        size_t const offset = 600 KB;   /* must be larger than window defined in cdict */
        size_t const start = jobSize + (offset-1);
        const BYTE* const srcToCopy = (const BYTE*)CNBuffer + start;
        BYTE* const dst = (BYTE*)CNBuffer + start - offset;
        DISPLAYLEVEL(3, "test%3i : compress %u bytes with multiple threads + dictionary : ", testNb++, (unsigned)srcSize);
        CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, 3) );
        CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_nbWorkers, nbWorkers) );
        CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_jobSize, jobSize) );
        assert(start > offset);
        assert(start + segLength < COMPRESSIBLE_NOISE_LENGTH);
        memcpy(dst, srcToCopy, segLength);   /* create a long repetition at long distance for job 2 */
        outBuff.dst = compressedBuffer;
        outBuff.size = compressedBufferSize;
        outBuff.pos = 0;
        inBuff.src = CNBuffer;
        inBuff.size = srcSize; assert(srcSize < COMPRESSIBLE_NOISE_LENGTH);
        inBuff.pos = 0;
    }
    {   ZSTD_compressionParameters const cParams = ZSTD_getCParams(1, 4 KB, dictionary.filled);   /* intentionally lies on estimatedSrcSize, to push cdict into targeting a small window size */
        ZSTD_CDict* const cdict = ZSTD_createCDict_advanced(dictionary.start, dictionary.filled, ZSTD_dlm_byRef, ZSTD_dct_fullDict, cParams, ZSTD_defaultCMem);
        DISPLAYLEVEL(5, "cParams.windowLog = %u : ", cParams.windowLog);
        CHECK_Z( ZSTD_CCtx_refCDict(zc, cdict) );
        CHECK_Z( ZSTD_compressStream2(zc, &outBuff, &inBuff, ZSTD_e_end) );
        CHECK_Z( ZSTD_CCtx_refCDict(zc, NULL) );  /* do not keep a reference to cdict, as its lifetime ends */
        ZSTD_freeCDict(cdict);
    }
    if (inBuff.pos != inBuff.size) goto _output_error;   /* entire input should be consumed */
    cSize = outBuff.pos;
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress large frame created from multiple threads + dictionary : ", testNb++);
    {   ZSTD_DStream* const dstream = ZSTD_createDCtx();
        ZSTD_frameHeader zfh;
        ZSTD_getFrameHeader(&zfh, compressedBuffer, cSize);
        DISPLAYLEVEL(5, "frame windowsize = %u : ", (unsigned)zfh.windowSize);
        outBuff.dst = decodedBuffer;
        outBuff.size = CNBufferSize;
        outBuff.pos = 0;
        inBuff.src = compressedBuffer;
        inBuff.pos = 0;
        CHECK_Z( ZSTD_initDStream_usingDict(dstream, dictionary.start, dictionary.filled) );
        inBuff.size = 1;  /* avoid shortcut to single-pass mode */
        CHECK_Z( ZSTD_decompressStream(dstream, &outBuff, &inBuff) );
        inBuff.size = cSize;
        CHECK_Z( ZSTD_decompressStream(dstream, &outBuff, &inBuff) );
        if (inBuff.pos != inBuff.size) goto _output_error;   /* entire input should be consumed */
        ZSTD_freeDStream(dstream);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : check dictionary FSE tables can represent every code : ", testNb++);
    {   unsigned const kMaxWindowLog = 24;
        unsigned value;
        ZSTD_compressionParameters cParams = ZSTD_getCParams(3, 1U << kMaxWindowLog, 1024);
        ZSTD_CDict* cdict;
        ZSTD_DDict* ddict;
        SEQ_stream seq = SEQ_initStream(0x87654321);
        SEQ_gen_type type;
        XXH64_state_t xxh;

        XXH64_reset(&xxh, 0);
        cParams.windowLog = kMaxWindowLog;
        cdict = ZSTD_createCDict_advanced(dictionary.start, dictionary.filled, ZSTD_dlm_byRef, ZSTD_dct_fullDict, cParams, ZSTD_defaultCMem);
        ddict = ZSTD_createDDict(dictionary.start, dictionary.filled);

        if (!cdict || !ddict) goto _output_error;

        ZSTD_CCtx_reset(zc, ZSTD_reset_session_only);
        ZSTD_resetDStream(zd);
        CHECK_Z(ZSTD_CCtx_refCDict(zc, cdict));
        CHECK_Z(ZSTD_initDStream_usingDDict(zd, ddict));
        CHECK_Z(ZSTD_DCtx_setParameter(zd, ZSTD_d_windowLogMax, kMaxWindowLog));
        /* Test all values < 300 */
        for (value = 0; value < 300; ++value) {
            for (type = (SEQ_gen_type)0; type < SEQ_gen_max; ++type) {
                CHECK_Z(SEQ_generateRoundTrip(zc, zd, &xxh, &seq, type, value));
            }
        }
        /* Test values 2^8 to 2^17 */
        for (value = (1 << 8); value < (1 << 17); value <<= 1) {
            for (type = (SEQ_gen_type)0; type < SEQ_gen_max; ++type) {
                CHECK_Z(SEQ_generateRoundTrip(zc, zd, &xxh, &seq, type, value));
                CHECK_Z(SEQ_generateRoundTrip(zc, zd, &xxh, &seq, type, value + (value >> 2)));
            }
        }
        /* Test offset values up to the max window log */
        for (value = 8; value <= kMaxWindowLog; ++value) {
            CHECK_Z(SEQ_generateRoundTrip(zc, zd, &xxh, &seq, SEQ_gen_of, (1U << value) - 1));
        }

        CHECK_Z(SEQ_roundTrip(zc, zd, &xxh, NULL, 0, ZSTD_e_end));
        CHECK(SEQ_digest(&seq) != XXH64_digest(&xxh), "SEQ XXH64 does not match");

        ZSTD_freeCDict(cdict);
        ZSTD_freeDDict(ddict);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_initCStream_srcSize sets requestedParams : ", testNb++);
    {   int level;
        CHECK_Z(ZSTD_initCStream_srcSize(zc, 11, ZSTD_CONTENTSIZE_UNKNOWN));
        CHECK_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_compressionLevel, &level));
        CHECK(level != 11, "Compression level does not match");
        CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
        CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, ZSTD_CONTENTSIZE_UNKNOWN) );
        CHECK_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_compressionLevel, &level));
        CHECK(level != 11, "Compression level does not match");
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_initCStream_advanced sets requestedParams : ", testNb++);
    {   ZSTD_parameters const params = ZSTD_getParams(9, 0, 0);
        CHECK_Z(ZSTD_initCStream_advanced(zc, NULL, 0, params, ZSTD_CONTENTSIZE_UNKNOWN));
        CHECK(badParameters(zc, params), "Compression parameters do not match");
        CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
        CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, ZSTD_CONTENTSIZE_UNKNOWN) );
        CHECK(badParameters(zc, params), "Compression parameters do not match");
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_c_srcSizeHint bounds : ", testNb++);
    ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters);
    CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_srcSizeHint, INT_MAX));
    {   int srcSizeHint;
        CHECK_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_srcSizeHint, &srcSizeHint));
        CHECK(!(srcSizeHint == INT_MAX), "srcSizeHint doesn't match");
    }
    CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(zc, ZSTD_c_srcSizeHint, -1)), "Out of range doesn't error");
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_lazy compress with hashLog = 29 and searchLog = 4 : ", testNb++);
    if (MEM_64bits()) {
        ZSTD_outBuffer out = { compressedBuffer, compressedBufferSize, 0 };
        ZSTD_inBuffer in = { CNBuffer, CNBufferSize, 0 };
        CHECK_Z(ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters));
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_strategy, ZSTD_lazy));
        /* Force enable the row based match finder */
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_useRowMatchFinder, ZSTD_ps_enable));
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_searchLog, 4));
        /* Set windowLog to 29 so the hashLog doesn't get sized down */
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_windowLog, 29));
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_hashLog, 29));
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_checksumFlag, 1));
        /* Compress with continue first so the hashLog doesn't get sized down */
        CHECK_Z(ZSTD_compressStream2(zc, &out, &in, ZSTD_e_continue));
        CHECK_Z(ZSTD_compressStream2(zc, &out, &in, ZSTD_e_end));
        cSize = out.pos;
        CHECK_Z(ZSTD_decompress(decodedBuffer, CNBufferSize, compressedBuffer, cSize));
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : Test offset == windowSize : ", testNb++);
    {
        int windowLog;
        int const kMaxWindowLog = bigTests ? 29 : 26;
        size_t const kNbSequences = 10000;
        size_t const kMaxSrcSize = (1u << kMaxWindowLog) + 10 * kNbSequences;
        char* src = calloc(kMaxSrcSize, 1);
        ZSTD_Sequence* sequences = malloc(sizeof(ZSTD_Sequence) * kNbSequences);
        for (windowLog = ZSTD_WINDOWLOG_MIN; windowLog <= kMaxWindowLog; ++windowLog) {
            size_t const srcSize = ((size_t)1 << windowLog) + 10 * (kNbSequences - 1);

            sequences[0].offset = 32;
            sequences[0].litLength = 32;
            sequences[0].matchLength = (1u << windowLog) - 32;
            sequences[0].rep = 0;
            {
                size_t i;
                for (i = 1; i < kNbSequences; ++i) {
                    sequences[i].offset = (1u << windowLog) - (FUZ_rand(&seed) % 8);
                    sequences[i].litLength = FUZ_rand(&seed) & 7;
                    sequences[i].matchLength = 10 - sequences[i].litLength;
                    sequences[i].rep = 0;
                }
            }

            CHECK_Z(ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters));
            CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_checksumFlag, 1));
            CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_minMatch, 3));
            CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_validateSequences, 1));
            CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_windowLog, windowLog));
            assert(srcSize <= kMaxSrcSize);
            cSize = ZSTD_compressSequences(zc, compressedBuffer, compressedBufferSize, sequences, kNbSequences, src, srcSize);
            CHECK_Z(cSize);
            CHECK_Z(ZSTD_DCtx_reset(zd, ZSTD_reset_session_and_parameters));
            CHECK_Z(ZSTD_DCtx_setParameter(zd, ZSTD_d_windowLogMax, windowLog))
            {
                ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
                size_t decompressedBytes = 0;
                for (;;) {
                    ZSTD_outBuffer out = {decodedBuffer, decodedBufferSize, 0};
                    size_t const ret = ZSTD_decompressStream(zd, &out, &in);
                    CHECK_Z(ret);
                    CHECK(decompressedBytes + out.pos > srcSize, "Output too large");
                    CHECK(memcmp(out.dst, src + decompressedBytes, out.pos), "Corrupted");
                    decompressedBytes += out.pos;
                    if (ret == 0) {
                        break;
                    }
                }
                CHECK(decompressedBytes != srcSize, "Output wrong size");
            }
        }
        free(sequences);
        free(src);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Overlen overwriting window data bug */
    DISPLAYLEVEL(3, "test%3i : wildcopy doesn't overwrite potential match data : ", testNb++);
    {   /* This test has a window size of 1024 bytes and consists of 3 blocks:
            1. 'a' repeated 517 times
            2. 'b' repeated 516 times
            3. a compressed block with no literals and 3 sequence commands:
                litlength = 0, offset = 24, match length = 24
                litlength = 0, offset = 24, match length = 3 (this one creates an overlength write of length 2*WILDCOPY_OVERLENGTH - 3)
                litlength = 0, offset = 1021, match length = 3 (this one will try to read from overwritten data if the buffer is too small) */

        const char* testCase =
            "\x28\xB5\x2F\xFD\x04\x00\x4C\x00\x00\x10\x61\x61\x01\x00\x00\x2A"
            "\x80\x05\x44\x00\x00\x08\x62\x01\x00\x00\x2A\x20\x04\x5D\x00\x00"
            "\x00\x03\x40\x00\x00\x64\x60\x27\xB0\xE0\x0C\x67\x62\xCE\xE0";
        ZSTD_DStream* const zds = ZSTD_createDStream();
        if (zds==NULL) goto _output_error;

        CHECK_Z( ZSTD_initDStream(zds) );
        inBuff.src = testCase;
        inBuff.size = 47;
        inBuff.pos = 0;
        outBuff.dst = decodedBuffer;
        outBuff.size = CNBufferSize;
        outBuff.pos = 0;

        while (inBuff.pos < inBuff.size) {
            CHECK_Z( ZSTD_decompressStream(zds, &outBuff, &inBuff) );
        }

        ZSTD_freeDStream(zds);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Small Sequence Section bug */
    DISPLAYLEVEL(3, "test%3i : decompress blocks with small sequences section : ", testNb++);
    {   /* This test consists of 3 blocks. Each block has one sequence.
            The sequence has literal length of 10, match length of 10 and offset of 10.
            The sequence value and compression mode for the blocks are following:
            The order of values are ll, ml, of.
              - First block  : (10, 7, 13) (rle, rle, rle)
                 - size of sequences section: 6 bytes (1 byte for nbSeq, 1 byte for encoding mode, 3 bytes for rle, 1 byte bitstream)
              - Second block : (10, 7, 1) (repeat, repeat, rle)
                 - size of sequences section: 4 bytes (1 byte for nbSeq, 1 byte for encoding mode, 1 bytes for rle, 1 byte bitstream)
              - Third block  : (10, 7, 1) (repeat, repeat, repeat)
                 - size of sequences section: 3 bytes (1 byte for nbSeq, 1 byte for encoding mode, 1 byte bitstream) */

        unsigned char compressed[] = {
            0x28, 0xb5, 0x2f, 0xfd, 0x24, 0x3c, 0x35, 0x01, 0x00, 0xf0, 0x85, 0x08,
            0xc2, 0xc4, 0x70, 0xcf, 0xd7, 0xc0, 0x96, 0x7e, 0x4c, 0x6b, 0xa9, 0x8b,
            0xbc, 0xc5, 0xb6, 0xd9, 0x7f, 0x4c, 0xf1, 0x05, 0xa6, 0x54, 0xef, 0xac,
            0x69, 0x94, 0x89, 0x1c, 0x03, 0x44, 0x0a, 0x07, 0x00, 0xb4, 0x04, 0x80,
            0x40, 0x0a, 0xa4
        };
        unsigned int compressedSize = 51;
        unsigned char decompressed[] = {
            0x85, 0x08, 0xc2, 0xc4, 0x70, 0xcf, 0xd7, 0xc0, 0x96, 0x7e, 0x85, 0x08,
            0xc2, 0xc4, 0x70, 0xcf, 0xd7, 0xc0, 0x96, 0x7e, 0x4c, 0x6b, 0xa9, 0x8b,
            0xbc, 0xc5, 0xb6, 0xd9, 0x7f, 0x4c, 0x4c, 0x6b, 0xa9, 0x8b, 0xbc, 0xc5,
            0xb6, 0xd9, 0x7f, 0x4c, 0xf1, 0x05, 0xa6, 0x54, 0xef, 0xac, 0x69, 0x94,
            0x89, 0x1c, 0xf1, 0x05, 0xa6, 0x54, 0xef, 0xac, 0x69, 0x94, 0x89, 0x1c
        };
        unsigned int decompressedSize = 60;

        ZSTD_DStream* const zds = ZSTD_createDStream();
        if (zds==NULL) goto _output_error;

        CHECK_Z( ZSTD_initDStream(zds) );
        inBuff.src = compressed;
        inBuff.size = compressedSize;
        inBuff.pos = 0;
        outBuff.dst = decodedBuffer;
        outBuff.size = CNBufferSize;
        outBuff.pos = 0;

        CHECK(ZSTD_decompressStream(zds, &outBuff, &inBuff) != 0,
              "Decompress did not reach the end of frame");
        CHECK(inBuff.pos != inBuff.size, "Decompress did not fully consume input");
        CHECK(outBuff.pos != decompressedSize, "Decompressed size does not match");
        CHECK(memcmp(outBuff.dst, decompressed, decompressedSize) != 0,
              "Decompressed data does not match");

        ZSTD_freeDStream(zds);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : raw block can be streamed: ", testNb++);
    {   size_t const inputSize = 10000;
        size_t const compCapacity = ZSTD_compressBound(inputSize);
        BYTE* const input = (BYTE*)malloc(inputSize);
        BYTE* const comp = (BYTE*)malloc(compCapacity);
        BYTE* const decomp = (BYTE*)malloc(inputSize);

        CHECK(input == NULL || comp == NULL || decomp == NULL, "failed to alloc buffers");

        RDG_genBuffer(input, inputSize, 0.0, 0.0, seed);
        {   size_t const compSize = ZSTD_compress(comp, compCapacity, input, inputSize, -(int)inputSize);
            ZSTD_inBuffer in = { comp, 0, 0 };
            ZSTD_outBuffer out = { decomp, 0, 0 };
            CHECK_Z(compSize);
            CHECK_Z( ZSTD_DCtx_reset(zd, ZSTD_reset_session_and_parameters) );
            while (in.size < compSize) {
                in.size = MIN(in.size + 100, compSize);
                while (in.pos < in.size) {
                    size_t const outPos = out.pos;
                    if (out.pos == out.size) {
                        out.size = MIN(out.size + 10, inputSize);
                    }
                    CHECK_Z( ZSTD_decompressStream(zd, &out, &in) );
                    CHECK(!(out.pos > outPos), "We are not streaming (no output generated)");
                }
            }
            CHECK(in.pos != compSize, "Not all input consumed!");
            CHECK(out.pos != inputSize, "Not all output produced!");
        }
        CHECK(memcmp(input, decomp, inputSize), "round trip failed!");

        free(input);
        free(comp);
        free(decomp);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : dictionary + uncompressible block + reusing tables checks offset table validity: ", testNb++);
    {   ZSTD_CDict* const cdict = ZSTD_createCDict_advanced(
            dictionary.start, dictionary.filled,
            ZSTD_dlm_byRef, ZSTD_dct_fullDict,
            ZSTD_getCParams(3, 0, dictionary.filled),
            ZSTD_defaultCMem);
        const size_t inbufsize = 2 * 128 * 1024; /* 2 blocks */
        const size_t outbufsize = ZSTD_compressBound(inbufsize);
        size_t inbufpos = 0;
        size_t cursegmentlen;
        BYTE *inbuf = (BYTE *)malloc(inbufsize);
        BYTE *outbuf = (BYTE *)malloc(outbufsize);
        BYTE *checkbuf = (BYTE *)malloc(inbufsize);
        size_t ret;

        CHECK(cdict == NULL, "failed to alloc cdict");
        CHECK(inbuf == NULL, "failed to alloc input buffer");

        /* first block is uncompressible */
        cursegmentlen = 128 * 1024;
        RDG_genBuffer(inbuf + inbufpos, cursegmentlen, 0., 0., seed);
        inbufpos += cursegmentlen;

        /* second block is compressible */
        cursegmentlen = 128 * 1024 - 256;
        RDG_genBuffer(inbuf + inbufpos, cursegmentlen, 0.05, 0., seed);
        inbufpos += cursegmentlen;

        /* and includes a very long backref */
        cursegmentlen = 128;
        memcpy(inbuf + inbufpos, (BYTE*)dictionary.start + 256, cursegmentlen);
        inbufpos += cursegmentlen;

        /* and includes a very long backref */
        cursegmentlen = 128;
        memcpy(inbuf + inbufpos, (BYTE*)dictionary.start + 128, cursegmentlen);
        inbufpos += cursegmentlen;

        ret = ZSTD_compress_usingCDict(zc, outbuf, outbufsize, inbuf, inbufpos, cdict);
        CHECK_Z(ret);

        ret = ZSTD_decompress_usingDict(zd, checkbuf, inbufsize, outbuf, ret, dictionary.start, dictionary.filled);
        CHECK_Z(ret);

        CHECK(memcmp(inbuf, checkbuf, inbufpos), "start and finish buffers don't match");

        ZSTD_freeCDict(cdict);
        free(inbuf);
        free(outbuf);
        free(checkbuf);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : dictionary + small blocks + reusing tables checks offset table validity: ", testNb++);
    {   ZSTD_CDict* const cdict = ZSTD_createCDict_advanced(
            dictionary.start, dictionary.filled,
            ZSTD_dlm_byRef, ZSTD_dct_fullDict,
            ZSTD_getCParams(3, 0, dictionary.filled),
            ZSTD_defaultCMem);
        ZSTD_outBuffer out = {compressedBuffer, compressedBufferSize, 0};
        int remainingInput = 256 * 1024;
        int offset;

        CHECK_Z(ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters));
        CHECK_Z(ZSTD_CCtx_refCDict(zc, cdict));
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_checksumFlag, 1));
        /* Write a bunch of 6 byte blocks */
        while (remainingInput > 0) {
          char testBuffer[6] = "\xAA\xAA\xAA\xAA\xAA\xAA";
          const size_t kSmallBlockSize = sizeof(testBuffer);
          ZSTD_inBuffer in = {testBuffer, kSmallBlockSize, 0};

          CHECK_Z(ZSTD_compressStream2(zc, &out, &in, ZSTD_e_flush));
          CHECK(in.pos != in.size, "input not fully consumed");
          remainingInput -= kSmallBlockSize;
        }
        /* Write several very long offset matches into the dictionary */
        for (offset = 1024; offset >= 0; offset -= 128) {
          ZSTD_inBuffer in = {(BYTE*)dictionary.start + offset, 128, 0};
          ZSTD_EndDirective flush = offset > 0 ? ZSTD_e_continue : ZSTD_e_end;
          CHECK_Z(ZSTD_compressStream2(zc, &out, &in, flush));
          CHECK(in.pos != in.size, "input not fully consumed");
        }
        /* Ensure decompression works */
        CHECK_Z(ZSTD_decompress_usingDict(zd, decodedBuffer, CNBufferSize, out.dst, out.pos, dictionary.start, dictionary.filled));

        ZSTD_freeCDict(cdict);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : Block-Level External Sequence Producer API: ", testNb++);
    {
        size_t const dstBufSize = ZSTD_compressBound(CNBufferSize);
        BYTE* const dstBuf = (BYTE*)malloc(ZSTD_compressBound(dstBufSize));
        size_t const checkBufSize = CNBufferSize;
        BYTE* const checkBuf = (BYTE*)malloc(checkBufSize);
        int enableFallback;
        EMF_testCase sequenceProducerState;

        CHECK(dstBuf == NULL || checkBuf == NULL, "allocation failed");

        CHECK_Z(ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters));

        /* Reference external matchfinder outside the test loop to
         * check that the reference is preserved across compressions */
        ZSTD_registerSequenceProducer(zc, &sequenceProducerState, zstreamSequenceProducer);

        for (enableFallback = 0; enableFallback <= 1; enableFallback++) {
            size_t testCaseId;
            size_t const numTestCases = 9;

            EMF_testCase const testCases[] = {
                EMF_ONE_BIG_SEQ,
                EMF_LOTS_OF_SEQS,
                EMF_ZERO_SEQS,
                EMF_BIG_ERROR,
                EMF_SMALL_ERROR,
                EMF_INVALID_OFFSET,
                EMF_INVALID_MATCHLEN,
                EMF_INVALID_LITLEN,
                EMF_INVALID_LAST_LITS
            };

            ZSTD_ErrorCode const errorCodes[] = {
                ZSTD_error_no_error,
                ZSTD_error_no_error,
                ZSTD_error_sequenceProducer_failed,
                ZSTD_error_sequenceProducer_failed,
                ZSTD_error_sequenceProducer_failed,
                ZSTD_error_externalSequences_invalid,
                ZSTD_error_externalSequences_invalid,
                ZSTD_error_externalSequences_invalid,
                ZSTD_error_externalSequences_invalid
            };

            for (testCaseId = 0; testCaseId < numTestCases; testCaseId++) {
                size_t res;

                int const compressionShouldSucceed = (
                    (errorCodes[testCaseId] == ZSTD_error_no_error) ||
                    (enableFallback && errorCodes[testCaseId] == ZSTD_error_sequenceProducer_failed)
                );

                int const testWithSequenceValidation = (
                    testCases[testCaseId] == EMF_INVALID_OFFSET
                );

                sequenceProducerState = testCases[testCaseId];

                ZSTD_CCtx_reset(zc, ZSTD_reset_session_only);
                CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_validateSequences, testWithSequenceValidation));
                CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_enableSeqProducerFallback, enableFallback));
                res = ZSTD_compress2(zc, dstBuf, dstBufSize, CNBuffer, CNBufferSize);

                if (compressionShouldSucceed) {
                    CHECK(ZSTD_isError(res), "EMF: Compression error: %s", ZSTD_getErrorName(res));
                    CHECK_Z(ZSTD_decompress(checkBuf, checkBufSize, dstBuf, res));
                    CHECK(memcmp(CNBuffer, checkBuf, CNBufferSize) != 0, "EMF: Corruption!");
                } else {
                    CHECK(!ZSTD_isError(res), "EMF: Should have raised an error!");
                    CHECK(
                        ZSTD_getErrorCode(res) != errorCodes[testCaseId],
                        "EMF: Wrong error code: %s", ZSTD_getErrorName(res)
                    );
                }
            }

            /* Test compression with external matchfinder + empty src buffer */
            {
                size_t res;
                sequenceProducerState = EMF_ZERO_SEQS;
                ZSTD_CCtx_reset(zc, ZSTD_reset_session_only);
                CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_enableSeqProducerFallback, enableFallback));
                res = ZSTD_compress2(zc, dstBuf, dstBufSize, CNBuffer, 0);
                CHECK(ZSTD_isError(res), "EMF: Compression error: %s", ZSTD_getErrorName(res));
                CHECK(ZSTD_decompress(checkBuf, checkBufSize, dstBuf, res) != 0, "EMF: Empty src round trip failed!");
            }
        }

        /* Test that reset clears the external matchfinder */
        CHECK_Z(ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters));
        sequenceProducerState = EMF_BIG_ERROR; /* ensure zstd will fail if the matchfinder wasn't cleared */
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_enableSeqProducerFallback, 0));
        CHECK_Z(ZSTD_compress2(zc, dstBuf, dstBufSize, CNBuffer, CNBufferSize));

        /* Test that registering mFinder == NULL clears the external matchfinder */
        ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters);
        ZSTD_registerSequenceProducer(zc, &sequenceProducerState, zstreamSequenceProducer);
        sequenceProducerState = EMF_BIG_ERROR; /* ensure zstd will fail if the matchfinder wasn't cleared */
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_enableSeqProducerFallback, 0));
        ZSTD_registerSequenceProducer(zc, NULL, NULL); /* clear the external matchfinder */
        CHECK_Z(ZSTD_compress2(zc, dstBuf, dstBufSize, CNBuffer, CNBufferSize));

        /* Test that external matchfinder doesn't interact with older APIs */
        ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters);
        ZSTD_registerSequenceProducer(zc, &sequenceProducerState, zstreamSequenceProducer);
        sequenceProducerState = EMF_BIG_ERROR; /* ensure zstd will fail if the matchfinder is used */
        CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_enableSeqProducerFallback, 0));
        CHECK_Z(ZSTD_compressCCtx(zc, dstBuf, dstBufSize, CNBuffer, CNBufferSize, 3));

        /* Test that compression returns the correct error with LDM */
        CHECK_Z(ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters));
        {
            size_t res;
            ZSTD_registerSequenceProducer(zc, &sequenceProducerState, zstreamSequenceProducer);
            CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_enableLongDistanceMatching, ZSTD_ps_enable));
            res = ZSTD_compress2(zc, dstBuf, dstBufSize, CNBuffer, CNBufferSize);
            CHECK(!ZSTD_isError(res), "EMF: Should have raised an error!");
            CHECK(
                ZSTD_getErrorCode(res) != ZSTD_error_parameter_combination_unsupported,
                "EMF: Wrong error code: %s", ZSTD_getErrorName(res)
            );
        }

#ifdef ZSTD_MULTITHREAD
        /* Test that compression returns the correct error with nbWorkers > 0 */
        CHECK_Z(ZSTD_CCtx_reset(zc, ZSTD_reset_session_and_parameters));
        {
            size_t res;
            ZSTD_registerSequenceProducer(zc, &sequenceProducerState, zstreamSequenceProducer);
            CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_nbWorkers, 1));
            res = ZSTD_compress2(zc, dstBuf, dstBufSize, CNBuffer, CNBufferSize);
            CHECK(!ZSTD_isError(res), "EMF: Should have raised an error!");
            CHECK(
                ZSTD_getErrorCode(res) != ZSTD_error_parameter_combination_unsupported,
                "EMF: Wrong error code: %s", ZSTD_getErrorName(res)
            );
        }
#endif

        free(dstBuf);
        free(checkBuf);
    }
    DISPLAYLEVEL(3, "OK \n");


    /* Test maxBlockSize cctx param functionality */
    DISPLAYLEVEL(3, "test%3i : Testing maxBlockSize PR#3418: ", testNb++);
    {
        ZSTD_CCtx* cctx = ZSTD_createCCtx();

        /* Quick test to make sure maxBlockSize bounds are enforced */
        assert(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_maxBlockSize, ZSTD_BLOCKSIZE_MAX_MIN - 1)));
        assert(ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_maxBlockSize, ZSTD_BLOCKSIZE_MAX + 1)));

        /* Test maxBlockSize < windowSize and windowSize < maxBlockSize*/
        {
            size_t srcSize = 2 << 10;
            void* const src = CNBuffer;
            size_t dstSize = ZSTD_compressBound(srcSize);
            void* const dst1 = compressedBuffer;
            void* const dst2 = (BYTE*)compressedBuffer + dstSize;
            size_t size1, size2;
            void* const checkBuf = malloc(srcSize);
            memset(src, 'x', srcSize);

            /* maxBlockSize = 1KB */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_maxBlockSize, 1u << 10));
            size1 = ZSTD_compress2(cctx, dst1, dstSize, src, srcSize);

            if (ZSTD_isError(size1)) goto _output_error;
            CHECK_Z(ZSTD_decompress(checkBuf, srcSize, dst1, size1));
            CHECK(memcmp(src, checkBuf, srcSize) != 0, "Corruption!");

            /* maxBlockSize = 3KB */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_maxBlockSize, 3u << 10));
            size2 = ZSTD_compress2(cctx, dst2, dstSize, src, srcSize);

            if (ZSTD_isError(size2)) goto _output_error;
            CHECK_Z(ZSTD_decompress(checkBuf, srcSize, dst2, size2));
            CHECK(memcmp(src, checkBuf, srcSize) != 0, "Corruption!");

            assert(size1 - size2 == 4); /* We add another RLE block with header + character */
            assert(memcmp(dst1, dst2, size2) != 0); /* Compressed output should not be equal */

            /* maxBlockSize = 1KB, windowLog = 10 */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_maxBlockSize, 1u << 10));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 10));
            size1 = ZSTD_compress2(cctx, dst1, dstSize, src, srcSize);

            if (ZSTD_isError(size1)) goto _output_error;
            CHECK_Z(ZSTD_decompress(checkBuf, srcSize, dst1, size1));
            CHECK(memcmp(src, checkBuf, srcSize) != 0, "Corruption!");

            /* maxBlockSize = 3KB, windowLog = 10 */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_maxBlockSize, 3u << 10));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 10));
            size2 = ZSTD_compress2(cctx, dst2, dstSize, src, srcSize);

            if (ZSTD_isError(size2)) goto _output_error;
            CHECK_Z(ZSTD_decompress(checkBuf, srcSize, dst2, size2));
            CHECK(memcmp(src, checkBuf, srcSize) != 0, "Corruption!");

            assert(size1 == size2);
            assert(memcmp(dst1, dst2, size1) == 0); /* Compressed output should be equal */

            free(checkBuf);
        }

        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);

        /* Test maxBlockSize = 0 is valid */
        {   size_t srcSize = 256 << 10;
            void* const src = CNBuffer;
            size_t dstSize = ZSTD_compressBound(srcSize);
            void* const dst1 = compressedBuffer;
            void* const dst2 = (BYTE*)compressedBuffer + dstSize;
            size_t size1, size2;
            void* const checkBuf = malloc(srcSize);

            /* maxBlockSize = 0 */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_maxBlockSize, 0));
            size1 = ZSTD_compress2(cctx, dst1, dstSize, src, srcSize);

            if (ZSTD_isError(size1)) goto _output_error;
            CHECK_Z(ZSTD_decompress(checkBuf, srcSize, dst1, size1));
            CHECK(memcmp(src, checkBuf, srcSize) != 0, "Corruption!");

            /* maxBlockSize = ZSTD_BLOCKSIZE_MAX */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_maxBlockSize, ZSTD_BLOCKSIZE_MAX));
            size2 = ZSTD_compress2(cctx, dst2, dstSize, src, srcSize);

            if (ZSTD_isError(size2)) goto _output_error;
            CHECK_Z(ZSTD_decompress(checkBuf, srcSize, dst2, size2));
            CHECK(memcmp(src, checkBuf, srcSize) != 0, "Corruption!");

            assert(size1 == size2);
            assert(memcmp(dst1, dst2, size1) == 0); /* Compressed output should be equal */
            free(checkBuf);
        }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Test Sequence Validation */
    DISPLAYLEVEL(3, "test%3i : Testing sequence validation: ", testNb++);
    {
        ZSTD_CCtx* cctx = ZSTD_createCCtx();

        /* Test minMatch >= 4, matchLength < 4 */
        {
            size_t srcSize = 11;
            void* const src = CNBuffer;
            size_t dstSize = ZSTD_compressBound(srcSize);
            void* const dst = compressedBuffer;
            size_t const kNbSequences = 4;
            ZSTD_Sequence* sequences = malloc(sizeof(ZSTD_Sequence) * kNbSequences);

            memset(src, 'x', srcSize);

            sequences[0] = (ZSTD_Sequence) {1, 1, 3, 0};
            sequences[1] = (ZSTD_Sequence) {1, 0, 3, 0};
            sequences[2] = (ZSTD_Sequence) {1, 0, 3, 0};
            sequences[3] = (ZSTD_Sequence) {0, 1, 0, 0};

            /* Test with sequence validation */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, 5));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_validateSequences, 1));

            cSize = ZSTD_compressSequences(cctx, dst, dstSize,
                                   sequences, kNbSequences,
                                   src, srcSize);

            CHECK(!ZSTD_isError(cSize), "Should throw an error"); /* maxNbSeq is too small and an assert will fail */
            CHECK(ZSTD_getErrorCode(cSize) != ZSTD_error_externalSequences_invalid, "Wrong error code: %s", ZSTD_getErrorName(cSize)); /* fails sequence validation */

            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);

            /* Test without sequence validation */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, 5));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_validateSequences, 0));

            cSize = ZSTD_compressSequences(cctx, dst, dstSize,
                                   sequences, kNbSequences,
                                   src, srcSize);

            CHECK(!ZSTD_isError(cSize), "Should throw an error"); /* maxNbSeq is too small and an assert will fail */
            CHECK(ZSTD_getErrorCode(cSize) != ZSTD_error_externalSequences_invalid, "Wrong error code: %s", ZSTD_getErrorName(cSize)); /* fails sequence validation */

            free(sequences);
        }

        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);


        /* Test with no block delim */
        {
            size_t srcSize = 4;
            void* const src = CNBuffer;
            size_t dstSize = ZSTD_compressBound(srcSize);
            void* const dst = compressedBuffer;
            size_t const kNbSequences = 1;
            ZSTD_Sequence* sequences = malloc(sizeof(ZSTD_Sequence) * kNbSequences);
            void* const checkBuf = malloc(srcSize);

            memset(src, 'x', srcSize);

            sequences[0] = (ZSTD_Sequence) {1, 1, 3, 0};

            /* Test with sequence validation */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, 3));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_noBlockDelimiters));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_validateSequences, 1));

            cSize = ZSTD_compressSequences(cctx, dst, dstSize,
                                   sequences, kNbSequences,
                                   src, srcSize);

            CHECK(ZSTD_isError(cSize), "Should not throw an error");
            CHECK_Z(ZSTD_decompress(checkBuf, srcSize, dst, cSize));
            CHECK(memcmp(src, checkBuf, srcSize) != 0, "Corruption!");

            free(sequences);
            free(checkBuf);
        }

        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);

        { /* Test case with two additional sequences */
            size_t srcSize = 19;
            void* const src = CNBuffer;
            size_t dstSize = ZSTD_compressBound(srcSize);
            void* const dst = compressedBuffer;
            size_t const kNbSequences = 7;
            ZSTD_Sequence* sequences = malloc(sizeof(ZSTD_Sequence) * kNbSequences);

            memset(src, 'x', srcSize);

            sequences[0] = (ZSTD_Sequence) {1, 1, 3, 0};
            sequences[1] = (ZSTD_Sequence) {1, 0, 3, 0};
            sequences[2] = (ZSTD_Sequence) {1, 0, 3, 0};
            sequences[3] = (ZSTD_Sequence) {1, 0, 3, 0};
            sequences[4] = (ZSTD_Sequence) {1, 0, 3, 0};
            sequences[5] = (ZSTD_Sequence) {1, 0, 3, 0};
            sequences[6] = (ZSTD_Sequence) {0, 0, 0, 0};

            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, 5));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_validateSequences, 1));

            cSize = ZSTD_compressSequences(cctx, dst, dstSize,
                                   sequences, kNbSequences,
                                   src, srcSize);

            CHECK(!ZSTD_isError(cSize), "Should throw an error"); /* maxNbSeq is too small and an assert will fail */
            CHECK(ZSTD_getErrorCode(cSize) != ZSTD_error_externalSequences_invalid, "Wrong error code: %s", ZSTD_getErrorName(cSize)); /* fails sequence validation */

            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);

            /* Test without sequence validation */
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_minMatch, 5));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_validateSequences, 0));

            cSize = ZSTD_compressSequences(cctx, dst, dstSize,
                                   sequences, kNbSequences,
                                   src, srcSize);

            CHECK(!ZSTD_isError(cSize), "Should throw an error"); /* maxNbSeq is too small and an assert will fail */
            CHECK(ZSTD_getErrorCode(cSize) != ZSTD_error_externalSequences_invalid, "Wrong error code: %s", ZSTD_getErrorName(cSize)); /* fails sequence validation */

            free(sequences);
        }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");


    DISPLAYLEVEL(3, "test%3i : Testing large offset with small window size: ", testNb++);
    {
        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        ZSTD_DCtx* dctx = ZSTD_createDCtx();

        /* Test large offset, small window size*/
        {
            size_t srcSize = 21;
            void* const src = CNBuffer;
            size_t dstSize = ZSTD_compressBound(srcSize);
            void* const dst = compressedBuffer;
            size_t const kNbSequences = 4;
            ZSTD_Sequence* sequences = malloc(sizeof(ZSTD_Sequence) * kNbSequences);
            void* const checkBuf = malloc(srcSize);
            const size_t largeDictSize = 1 << 25;
            ZSTD_CDict* cdict = NULL;
            ZSTD_DDict* ddict = NULL;

            /* Generate large dictionary */
            void* dictBuffer = calloc(largeDictSize, 1);
            ZSTD_compressionParameters cParams = ZSTD_getCParams(1, srcSize, largeDictSize);
            cParams.minMatch = ZSTD_MINMATCH_MIN;
            cParams.hashLog = ZSTD_HASHLOG_MIN;
            cParams.chainLog = ZSTD_CHAINLOG_MIN;

            cdict = ZSTD_createCDict_advanced(dictBuffer, largeDictSize, ZSTD_dlm_byRef, ZSTD_dct_rawContent, cParams, ZSTD_defaultCMem);
            ddict = ZSTD_createDDict_advanced(dictBuffer, largeDictSize, ZSTD_dlm_byRef, ZSTD_dct_rawContent, ZSTD_defaultCMem);

            ZSTD_CCtx_refCDict(cctx, cdict);
            ZSTD_DCtx_refDDict(dctx, ddict);

            sequences[0] = (ZSTD_Sequence) {3, 3, 3, 0};
            sequences[1] = (ZSTD_Sequence) {1 << 25, 0, 3, 0};
            sequences[2] = (ZSTD_Sequence) {1 << 25, 0, 9, 0};
            sequences[3] = (ZSTD_Sequence) {3, 0, 3, 0};

            cSize = ZSTD_compressSequences(cctx, dst, dstSize,
                                   sequences, kNbSequences,
                                   src, srcSize);

            CHECK(ZSTD_isError(cSize), "Should not throw an error");

            {
                size_t dSize = ZSTD_decompressDCtx(dctx, checkBuf, srcSize, dst, cSize);
                CHECK(ZSTD_isError(dSize), "Should not throw an error");
                CHECK(memcmp(src, checkBuf, srcSize) != 0, "Corruption!");
            }

            free(sequences);
            free(checkBuf);
            free(dictBuffer);
            ZSTD_freeCDict(cdict);
            ZSTD_freeDDict(ddict);
        }
        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

_end:
    FUZ_freeDictionary(dictionary);
    ZSTD_freeCStream(zc);
    ZSTD_freeDStream(zd);
    ZSTD_freeCCtx(mtctx);
    free(CNBuffer);
    free(compressedBuffer);
    free(decodedBuffer);
    return testResult;

_output_error:
    testResult = 1;
    DISPLAY("Error detected in Unit tests ! \n");
    goto _end;
}


/* ======   Fuzzer tests   ====== */

static size_t findDiff(const void* buf1, const void* buf2, size_t max)
{
    const BYTE* b1 = (const BYTE*)buf1;
    const BYTE* b2 = (const BYTE*)buf2;
    size_t u;
    for (u=0; u<max; u++) {
        if (b1[u] != b2[u]) break;
    }
    if (u==max) {
        DISPLAY("=> No difference detected within %u bytes \n", (unsigned)max);
        return u;
    }
    DISPLAY("Error at position %u / %u \n", (unsigned)u, (unsigned)max);
    if (u>=3)
        DISPLAY(" %02X %02X %02X ",
                b1[u-3], b1[u-2], b1[u-1]);
    DISPLAY(" :%02X:  %02X %02X %02X %02X %02X \n",
            b1[u], b1[u+1], b1[u+2], b1[u+3], b1[u+4], b1[u+5]);
    if (u>=3)
        DISPLAY(" %02X %02X %02X ",
                b2[u-3], b2[u-2], b2[u-1]);
    DISPLAY(" :%02X:  %02X %02X %02X %02X %02X \n",
            b2[u], b2[u+1], b2[u+2], b2[u+3], b2[u+4], b2[u+5]);
    return u;
}

static size_t FUZ_rLogLength(U32* seed, U32 logLength)
{
    size_t const lengthMask = ((size_t)1 << logLength) - 1;
    return (lengthMask+1) + (FUZ_rand(seed) & lengthMask);
}

static size_t FUZ_randomLength(U32* seed, U32 maxLog)
{
    U32 const logLength = FUZ_rand(seed) % maxLog;
    return FUZ_rLogLength(seed, logLength);
}

/* Return value in range minVal <= v <= maxVal */
static U32 FUZ_randomClampedLength(U32* seed, U32 minVal, U32 maxVal)
{
    U32 const mod = maxVal < minVal ? 1 : (maxVal + 1) - minVal;
    return (U32)((FUZ_rand(seed) % mod) + minVal);
}

static int fuzzerTests(U32 seed, unsigned nbTests, unsigned startTest, double compressibility, int bigTests)
{
    U32 const maxSrcLog = bigTests ? 24 : 22;
    static const U32 maxSampleLog = 19;
    size_t const srcBufferSize = (size_t)1<<maxSrcLog;
    BYTE* cNoiseBuffer[5];
    size_t const copyBufferSize = srcBufferSize + (1<<maxSampleLog);
    BYTE*  const copyBuffer = (BYTE*)malloc (copyBufferSize);
    size_t const cBufferSize = ZSTD_compressBound(srcBufferSize);
    BYTE*  const cBuffer = (BYTE*)malloc (cBufferSize);
    size_t const dstBufferSize = srcBufferSize;
    BYTE*  const dstBuffer = (BYTE*)malloc (dstBufferSize);
    U32 result = 0;
    unsigned testNb = 0;
    U32 coreSeed = seed;
    ZSTD_CStream* zc = ZSTD_createCStream();   /* will be re-created sometimes */
    ZSTD_DStream* zd = ZSTD_createDStream();   /* will be re-created sometimes */
    ZSTD_DStream* const zd_noise = ZSTD_createDStream();
    UTIL_time_t const startClock = UTIL_getTime();
    const BYTE* dict = NULL;  /* can keep same dict on 2 consecutive tests */
    size_t dictSize = 0;
    U32 oldTestLog = 0;
    U32 const cLevelMax = bigTests ? (U32)ZSTD_maxCLevel() : g_cLevelMax_smallTests;

    /* allocations */
    cNoiseBuffer[0] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[1] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[2] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[3] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[4] = (BYTE*)malloc (srcBufferSize);
    CHECK (!cNoiseBuffer[0] || !cNoiseBuffer[1] || !cNoiseBuffer[2] || !cNoiseBuffer[3] || !cNoiseBuffer[4] ||
           !copyBuffer || !dstBuffer || !cBuffer || !zc || !zd || !zd_noise ,
           "Not enough memory, fuzzer tests cancelled");

    /* Create initial samples */
    RDG_genBuffer(cNoiseBuffer[0], srcBufferSize, 0.00, 0., coreSeed);    /* pure noise */
    RDG_genBuffer(cNoiseBuffer[1], srcBufferSize, 0.05, 0., coreSeed);    /* barely compressible */
    RDG_genBuffer(cNoiseBuffer[2], srcBufferSize, compressibility, 0., coreSeed);
    RDG_genBuffer(cNoiseBuffer[3], srcBufferSize, 0.95, 0., coreSeed);    /* highly compressible */
    RDG_genBuffer(cNoiseBuffer[4], srcBufferSize, 1.00, 0., coreSeed);    /* sparse content */
    memset(copyBuffer, 0x65, copyBufferSize);                             /* make copyBuffer considered initialized */
    ZSTD_initDStream_usingDict(zd, NULL, 0);  /* ensure at least one init */

    /* catch up testNb */
    for (testNb=1; testNb < startTest; testNb++)
        FUZ_rand(&coreSeed);

    /* test loop */
    for ( ; (testNb <= nbTests) || (UTIL_clockSpanMicro(startClock) < g_clockTime) ; testNb++ ) {
        U32 lseed;
        const BYTE* srcBuffer;
        size_t totalTestSize, totalGenSize, cSize;
        XXH64_state_t xxhState;
        U64 crcOrig;
        U32 resetAllowed = 1;
        size_t maxTestSize;

        /* init */
        FUZ_rand(&coreSeed);
        lseed = coreSeed ^ prime32;
        if (nbTests >= testNb) {
            DISPLAYUPDATE(2, "\r%6u/%6u    ", testNb, nbTests);
        } else {
            DISPLAYUPDATE(2, "\r%6u        ", testNb);
        }

        /* states full reset (deliberately not synchronized) */
        /* some issues can only happen when reusing states */
        if ((FUZ_rand(&lseed) & 0xFF) == 131) {
            ZSTD_freeCStream(zc);
            zc = ZSTD_createCStream();
            CHECK(zc==NULL, "ZSTD_createCStream : allocation error");
            resetAllowed=0;
        }
        if ((FUZ_rand(&lseed) & 0xFF) == 132) {
            ZSTD_freeDStream(zd);
            zd = ZSTD_createDStream();
            CHECK(zd==NULL, "ZSTD_createDStream : allocation error");
            CHECK_Z( ZSTD_initDStream_usingDict(zd, NULL, 0) );  /* ensure at least one init */
        }

        /* srcBuffer selection [0-4] */
        {   U32 buffNb = FUZ_rand(&lseed) & 0x7F;
            if (buffNb & 7) buffNb=2;   /* most common : compressible (P) */
            else {
                buffNb >>= 3;
                if (buffNb & 7) {
                    const U32 tnb[2] = { 1, 3 };   /* barely/highly compressible */
                    buffNb = tnb[buffNb >> 3];
                } else {
                    const U32 tnb[2] = { 0, 4 };   /* not compressible / sparse */
                    buffNb = tnb[buffNb >> 3];
            }   }
            srcBuffer = cNoiseBuffer[buffNb];
        }

        /* compression init */
        if ((FUZ_rand(&lseed)&1) /* at beginning, to keep same nb of rand */
            && oldTestLog /* at least one test happened */ && resetAllowed) {
            maxTestSize = FUZ_randomLength(&lseed, oldTestLog+2);
            maxTestSize = MIN(maxTestSize, srcBufferSize-16);
            {   U64 const pledgedSrcSize = (FUZ_rand(&lseed) & 3) ? ZSTD_CONTENTSIZE_UNKNOWN : maxTestSize;
                CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
                CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, pledgedSrcSize) );
            }
        } else {
            U32 const testLog = FUZ_rand(&lseed) % maxSrcLog;
            U32 const dictLog = FUZ_rand(&lseed) % maxSrcLog;
            U32 const cLevelCandidate = ( FUZ_rand(&lseed) %
                                (ZSTD_maxCLevel() -
                                (MAX(testLog, dictLog) / 3)))
                                 + 1;
            U32 const cLevel = MIN(cLevelCandidate, cLevelMax);
            maxTestSize = FUZ_rLogLength(&lseed, testLog);
            oldTestLog = testLog;
            /* random dictionary selection */
            dictSize  = ((FUZ_rand(&lseed)&7)==1) ? FUZ_rLogLength(&lseed, dictLog) : 0;
            {   size_t const dictStart = FUZ_rand(&lseed) % (srcBufferSize - dictSize);
                dict = srcBuffer + dictStart;
            }
            {   U64 const pledgedSrcSize = (FUZ_rand(&lseed) & 3) ? ZSTD_CONTENTSIZE_UNKNOWN : maxTestSize;
                CHECK_Z( ZSTD_CCtx_reset(zc, ZSTD_reset_session_only) );
                CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, cLevel) );
                CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_checksumFlag, FUZ_rand(&lseed) & 1) );
                CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_contentSizeFlag, FUZ_rand(&lseed) & 1) );
                CHECK_Z( ZSTD_CCtx_setParameter(zc, ZSTD_c_dictIDFlag, FUZ_rand(&lseed) & 1) );
                CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, pledgedSrcSize) );
                CHECK_Z( ZSTD_CCtx_loadDictionary(zc, dict, dictSize) );
        }   }

        /* multi-segments compression test */
        XXH64_reset(&xxhState, 0);
        {   ZSTD_outBuffer outBuff = { cBuffer, cBufferSize, 0 } ;
            cSize=0;
            totalTestSize=0;
            while(totalTestSize < maxTestSize) {
                /* compress random chunks into randomly sized dst buffers */
                {   size_t const randomSrcSize = FUZ_randomLength(&lseed, maxSampleLog);
                    size_t const srcSize = MIN(maxTestSize-totalTestSize, randomSrcSize);
                    size_t const srcStart = FUZ_rand(&lseed) % (srcBufferSize - srcSize);
                    size_t const randomDstSize = FUZ_randomLength(&lseed, maxSampleLog);
                    size_t const dstBuffSize = MIN(cBufferSize - cSize, randomDstSize);
                    ZSTD_inBuffer inBuff = { srcBuffer+srcStart, srcSize, 0 };
                    outBuff.size = outBuff.pos + dstBuffSize;

                    CHECK_Z( ZSTD_compressStream(zc, &outBuff, &inBuff) );

                    XXH64_update(&xxhState, srcBuffer+srcStart, inBuff.pos);
                    memcpy(copyBuffer+totalTestSize, srcBuffer+srcStart, inBuff.pos);
                    totalTestSize += inBuff.pos;
                }

                /* random flush operation, to mess around */
                if ((FUZ_rand(&lseed) & 15) == 0) {
                    size_t const randomDstSize = FUZ_randomLength(&lseed, maxSampleLog);
                    size_t const adjustedDstSize = MIN(cBufferSize - cSize, randomDstSize);
                    outBuff.size = outBuff.pos + adjustedDstSize;
                    CHECK_Z( ZSTD_flushStream(zc, &outBuff) );
            }   }

            /* final frame epilogue */
            {   size_t remainingToFlush = (size_t)(-1);
                while (remainingToFlush) {
                    size_t const randomDstSize = FUZ_randomLength(&lseed, maxSampleLog);
                    size_t const adjustedDstSize = MIN(cBufferSize - cSize, randomDstSize);
                    outBuff.size = outBuff.pos + adjustedDstSize;
                    remainingToFlush = ZSTD_endStream(zc, &outBuff);
                    CHECK (ZSTD_isError(remainingToFlush), "end error : %s", ZSTD_getErrorName(remainingToFlush));
            }   }
            crcOrig = XXH64_digest(&xxhState);
            cSize = outBuff.pos;
        }

        /* multi - fragments decompression test */
        if (!dictSize /* don't reset if dictionary : could be different */ && (FUZ_rand(&lseed) & 1)) {
            CHECK_Z ( ZSTD_resetDStream(zd) );
        } else {
            CHECK_Z ( ZSTD_initDStream_usingDict(zd, dict, dictSize) );
        }
        {   size_t decompressionResult = 1;
            ZSTD_inBuffer  inBuff = { cBuffer, cSize, 0 };
            ZSTD_outBuffer outBuff= { dstBuffer, dstBufferSize, 0 };
            for (totalGenSize = 0 ; decompressionResult ; ) {
                size_t const readCSrcSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const randomDstSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const dstBuffSize = MIN(dstBufferSize - totalGenSize, randomDstSize);
                inBuff.size = inBuff.pos + readCSrcSize;
                outBuff.size = outBuff.pos + dstBuffSize;
                decompressionResult = ZSTD_decompressStream(zd, &outBuff, &inBuff);
                if (ZSTD_getErrorCode(decompressionResult) == ZSTD_error_checksum_wrong) {
                    DISPLAY("checksum error : \n");
                    findDiff(copyBuffer, dstBuffer, totalTestSize);
                }
                CHECK( ZSTD_isError(decompressionResult), "decompression error : %s",
                       ZSTD_getErrorName(decompressionResult) );
            }
            CHECK (decompressionResult != 0, "frame not fully decoded");
            CHECK (outBuff.pos != totalTestSize, "decompressed data : wrong size (%u != %u)",
                    (unsigned)outBuff.pos, (unsigned)totalTestSize);
            CHECK (inBuff.pos != cSize, "compressed data should be fully read")
            {   U64 const crcDest = XXH64(dstBuffer, totalTestSize, 0);
                if (crcDest!=crcOrig) findDiff(copyBuffer, dstBuffer, totalTestSize);
                CHECK (crcDest!=crcOrig, "decompressed data corrupted");
        }   }

        /*=====   noisy/erroneous src decompression test   =====*/

        /* add some noise */
        {   U32 const nbNoiseChunks = (FUZ_rand(&lseed) & 7) + 2;
            U32 nn; for (nn=0; nn<nbNoiseChunks; nn++) {
                size_t const randomNoiseSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const noiseSize  = MIN((cSize/3) , randomNoiseSize);
                size_t const noiseStart = FUZ_rand(&lseed) % (srcBufferSize - noiseSize);
                size_t const cStart = FUZ_rand(&lseed) % (cSize - noiseSize);
                memcpy(cBuffer+cStart, srcBuffer+noiseStart, noiseSize);
        }   }

        /* try decompression on noisy data */
        CHECK_Z( ZSTD_initDStream(zd_noise) );   /* note : no dictionary */
        {   ZSTD_inBuffer  inBuff = { cBuffer, cSize, 0 };
            ZSTD_outBuffer outBuff= { dstBuffer, dstBufferSize, 0 };
            while (outBuff.pos < dstBufferSize) {
                size_t const randomCSrcSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const randomDstSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const adjustedDstSize = MIN(dstBufferSize - outBuff.pos, randomDstSize);
                size_t const adjustedCSrcSize = MIN(cSize - inBuff.pos, randomCSrcSize);
                outBuff.size = outBuff.pos + adjustedDstSize;
                inBuff.size  = inBuff.pos + adjustedCSrcSize;
                {   size_t const decompressError = ZSTD_decompressStream(zd, &outBuff, &inBuff);
                    if (ZSTD_isError(decompressError)) break;   /* error correctly detected */
                    /* No forward progress possible */
                    if (outBuff.pos < outBuff.size && inBuff.pos == cSize) break;
    }   }   }   }
    DISPLAY("\r%u fuzzer tests completed   \n", testNb);

_cleanup:
    ZSTD_freeCStream(zc);
    ZSTD_freeDStream(zd);
    ZSTD_freeDStream(zd_noise);
    free(cNoiseBuffer[0]);
    free(cNoiseBuffer[1]);
    free(cNoiseBuffer[2]);
    free(cNoiseBuffer[3]);
    free(cNoiseBuffer[4]);
    free(copyBuffer);
    free(cBuffer);
    free(dstBuffer);
    return result;

_output_error:
    result = 1;
    goto _cleanup;
}

/** If useOpaqueAPI, sets param in cctxParams.
 *  Otherwise, sets the param in zc. */
static size_t setCCtxParameter(ZSTD_CCtx* zc, ZSTD_CCtx_params* cctxParams,
                               ZSTD_cParameter param, unsigned value,
                               int useOpaqueAPI)
{
    if (useOpaqueAPI) {
        return ZSTD_CCtxParams_setParameter(cctxParams, param, value);
    } else {
        return ZSTD_CCtx_setParameter(zc, param, value);
    }
}

/* Tests for ZSTD_compress_generic() API */
static int fuzzerTests_newAPI(U32 seed, int nbTests, int startTest,
                              double compressibility, int bigTests)
{
    U32 const maxSrcLog = bigTests ? 24 : 22;
    static const U32 maxSampleLog = 19;
    size_t const srcBufferSize = (size_t)1<<maxSrcLog;
    BYTE* cNoiseBuffer[5];
    size_t const copyBufferSize= srcBufferSize + (1<<maxSampleLog);
    BYTE*  const copyBuffer = (BYTE*)malloc (copyBufferSize);
    size_t const cBufferSize   = ZSTD_compressBound(srcBufferSize);
    BYTE*  const cBuffer = (BYTE*)malloc (cBufferSize);
    size_t const dstBufferSize = srcBufferSize;
    BYTE*  const dstBuffer = (BYTE*)malloc (dstBufferSize);
    U32 result = 0;
    int testNb = 0;
    U32 coreSeed = seed;
    ZSTD_CCtx* zc = ZSTD_createCCtx();   /* will be reset sometimes */
    ZSTD_DStream* zd = ZSTD_createDStream();   /* will be reset sometimes */
    ZSTD_DStream* const zd_noise = ZSTD_createDStream();
    UTIL_time_t const startClock = UTIL_getTime();
    const BYTE* dict = NULL;   /* can keep same dict on 2 consecutive tests */
    size_t dictSize = 0;
    U32 oldTestLog = 0;
    U32 windowLogMalus = 0;   /* can survive between 2 loops */
    U32 const cLevelMax = bigTests ? (U32)ZSTD_maxCLevel()-1 : g_cLevelMax_smallTests;
    U32 const nbThreadsMax = bigTests ? 4 : 2;
    ZSTD_CCtx_params* cctxParams = ZSTD_createCCtxParams();

    /* allocations */
    cNoiseBuffer[0] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[1] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[2] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[3] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[4] = (BYTE*)malloc (srcBufferSize);
    CHECK (!cNoiseBuffer[0] || !cNoiseBuffer[1] || !cNoiseBuffer[2] || !cNoiseBuffer[3] || !cNoiseBuffer[4] ||
           !copyBuffer || !dstBuffer || !cBuffer || !zc || !zd || !zd_noise ,
           "Not enough memory, fuzzer tests cancelled");

    /* Create initial samples */
    RDG_genBuffer(cNoiseBuffer[0], srcBufferSize, 0.00, 0., coreSeed);    /* pure noise */
    RDG_genBuffer(cNoiseBuffer[1], srcBufferSize, 0.05, 0., coreSeed);    /* barely compressible */
    RDG_genBuffer(cNoiseBuffer[2], srcBufferSize, compressibility, 0., coreSeed);
    RDG_genBuffer(cNoiseBuffer[3], srcBufferSize, 0.95, 0., coreSeed);    /* highly compressible */
    RDG_genBuffer(cNoiseBuffer[4], srcBufferSize, 1.00, 0., coreSeed);    /* sparse content */
    memset(copyBuffer, 0x65, copyBufferSize);                             /* make copyBuffer considered initialized */
    CHECK_Z( ZSTD_initDStream_usingDict(zd, NULL, 0) );   /* ensure at least one init */

    /* catch up testNb */
    for (testNb=1; testNb < startTest; testNb++)
        FUZ_rand(&coreSeed);

    /* test loop */
    for ( ; (testNb <= nbTests) || (UTIL_clockSpanMicro(startClock) < g_clockTime) ; testNb++ ) {
        U32 lseed;
        int opaqueAPI;
        const BYTE* srcBuffer;
        size_t totalTestSize, totalGenSize, cSize;
        XXH64_state_t xxhState;
        U64 crcOrig;
        U32 resetAllowed = 1;
        size_t maxTestSize;
        ZSTD_parameters savedParams;
        int isRefPrefix = 0;
        U64 pledgedSrcSize = ZSTD_CONTENTSIZE_UNKNOWN;

        /* init */
        if (nbTests >= testNb) { DISPLAYUPDATE(2, "\r%6u/%6u    ", testNb, nbTests); }
        else { DISPLAYUPDATE(2, "\r%6u          ", testNb); }
        FUZ_rand(&coreSeed);
        lseed = coreSeed ^ prime32;
        DISPLAYLEVEL(5, " ***  Test %u  *** \n", testNb);
        opaqueAPI = FUZ_rand(&lseed) & 1;

        /* states full reset (deliberately not synchronized) */
        /* some issues can only happen when reusing states */
        if ((FUZ_rand(&lseed) & 0xFF) == 131) {
            DISPLAYLEVEL(5, "Creating new context \n");
            ZSTD_freeCCtx(zc);
            zc = ZSTD_createCCtx();
            CHECK(zc == NULL, "ZSTD_createCCtx allocation error");
            resetAllowed = 0;
        }
        if ((FUZ_rand(&lseed) & 0xFF) == 132) {
            ZSTD_freeDStream(zd);
            zd = ZSTD_createDStream();
            CHECK(zd == NULL, "ZSTD_createDStream allocation error");
            ZSTD_initDStream_usingDict(zd, NULL, 0);  /* ensure at least one init */
        }

        /* srcBuffer selection [0-4] */
        {   U32 buffNb = FUZ_rand(&lseed) & 0x7F;
            if (buffNb & 7) buffNb=2;   /* most common : compressible (P) */
            else {
                buffNb >>= 3;
                if (buffNb & 7) {
                    const U32 tnb[2] = { 1, 3 };   /* barely/highly compressible */
                    buffNb = tnb[buffNb >> 3];
                } else {
                    const U32 tnb[2] = { 0, 4 };   /* not compressible / sparse */
                    buffNb = tnb[buffNb >> 3];
            }   }
            srcBuffer = cNoiseBuffer[buffNb];
        }

        /* compression init */
        CHECK_Z( ZSTD_CCtx_loadDictionary(zc, NULL, 0) );   /* cancel previous dict /*/
        if ((FUZ_rand(&lseed)&1) /* at beginning, to keep same nb of rand */
          && oldTestLog   /* at least one test happened */
          && resetAllowed) {
            /* just set a compression level */
            maxTestSize = FUZ_randomLength(&lseed, oldTestLog+2);
            if (maxTestSize >= srcBufferSize) maxTestSize = srcBufferSize-1;
            {   int const compressionLevel = (FUZ_rand(&lseed) % 5) + 1;
                DISPLAYLEVEL(5, "t%u : compression level : %i \n", testNb, compressionLevel);
                CHECK_Z (setCCtxParameter(zc, cctxParams, ZSTD_c_compressionLevel, compressionLevel, opaqueAPI) );
            }
        } else {
            U32 const testLog = FUZ_rand(&lseed) % maxSrcLog;
            U32 const dictLog = FUZ_rand(&lseed) % maxSrcLog;
            U32 const cLevelCandidate = (FUZ_rand(&lseed) %
                               (ZSTD_maxCLevel() -
                               (MAX(testLog, dictLog) / 2))) +
                               1;
            int const cLevel = MIN(cLevelCandidate, cLevelMax);
            DISPLAYLEVEL(5, "t%i: base cLevel : %u \n", testNb, cLevel);
            maxTestSize = FUZ_rLogLength(&lseed, testLog);
            DISPLAYLEVEL(5, "t%i: maxTestSize : %u \n", testNb, (unsigned)maxTestSize);
            oldTestLog = testLog;
            /* random dictionary selection */
            dictSize  = ((FUZ_rand(&lseed)&63)==1) ? FUZ_rLogLength(&lseed, dictLog) : 0;
            {   size_t const dictStart = FUZ_rand(&lseed) % (srcBufferSize - dictSize);
                dict = srcBuffer + dictStart;
                if (!dictSize) dict=NULL;
            }
            pledgedSrcSize = (FUZ_rand(&lseed) & 3) ? ZSTD_CONTENTSIZE_UNKNOWN : maxTestSize;
            {   ZSTD_compressionParameters cParams = ZSTD_getCParams(cLevel, pledgedSrcSize, dictSize);
                const U32 windowLogMax = bigTests ? 24 : 20;
                const U32 searchLogMax = bigTests ? 15 : 13;
                if (dictSize)
                    DISPLAYLEVEL(5, "t%u: with dictionary of size : %zu \n", testNb, dictSize);

                /* mess with compression parameters */
                cParams.windowLog += (FUZ_rand(&lseed) & 3) - 1;
                cParams.windowLog = MIN(windowLogMax, cParams.windowLog);
                cParams.hashLog += (FUZ_rand(&lseed) & 3) - 1;
                cParams.chainLog += (FUZ_rand(&lseed) & 3) - 1;
                cParams.searchLog += (FUZ_rand(&lseed) & 3) - 1;
                cParams.searchLog = MIN(searchLogMax, cParams.searchLog);
                cParams.minMatch += (FUZ_rand(&lseed) & 3) - 1;
                cParams.targetLength = (U32)((cParams.targetLength + 1 ) * (0.5 + ((double)(FUZ_rand(&lseed) & 127) / 128)));
                cParams = ZSTD_adjustCParams(cParams, pledgedSrcSize, dictSize);

                if (FUZ_rand(&lseed) & 1) {
                    DISPLAYLEVEL(5, "t%u: windowLog : %u \n", testNb, cParams.windowLog);
                    CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_windowLog, cParams.windowLog, opaqueAPI) );
                    assert(cParams.windowLog >= ZSTD_WINDOWLOG_MIN);   /* guaranteed by ZSTD_adjustCParams() */
                    windowLogMalus = (cParams.windowLog - ZSTD_WINDOWLOG_MIN) / 5;
                }
                if (FUZ_rand(&lseed) & 1) {
                    DISPLAYLEVEL(5, "t%u: hashLog : %u \n", testNb, cParams.hashLog);
                    CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_hashLog, cParams.hashLog, opaqueAPI) );
                }
                if (FUZ_rand(&lseed) & 1) {
                    DISPLAYLEVEL(5, "t%u: chainLog : %u \n", testNb, cParams.chainLog);
                    CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_chainLog, cParams.chainLog, opaqueAPI) );
                }
                if (FUZ_rand(&lseed) & 1) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_searchLog, cParams.searchLog, opaqueAPI) );
                if (FUZ_rand(&lseed) & 1) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_minMatch, cParams.minMatch, opaqueAPI) );
                if (FUZ_rand(&lseed) & 1) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_targetLength, cParams.targetLength, opaqueAPI) );

                /* mess with long distance matching parameters */
                if (bigTests) {
                    if (FUZ_rand(&lseed) & 1) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_enableLongDistanceMatching, FUZ_randomClampedLength(&lseed, ZSTD_ps_auto, ZSTD_ps_disable), opaqueAPI) );
                    if (FUZ_rand(&lseed) & 3) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_ldmHashLog, FUZ_randomClampedLength(&lseed, ZSTD_HASHLOG_MIN, 23), opaqueAPI) );
                    if (FUZ_rand(&lseed) & 3) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_ldmMinMatch, FUZ_randomClampedLength(&lseed, ZSTD_LDM_MINMATCH_MIN, ZSTD_LDM_MINMATCH_MAX), opaqueAPI) );
                    if (FUZ_rand(&lseed) & 3) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_ldmBucketSizeLog, FUZ_randomClampedLength(&lseed, ZSTD_LDM_BUCKETSIZELOG_MIN, ZSTD_LDM_BUCKETSIZELOG_MAX), opaqueAPI) );
                    if (FUZ_rand(&lseed) & 3) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_ldmHashRateLog, FUZ_randomClampedLength(&lseed, ZSTD_LDM_HASHRATELOG_MIN, ZSTD_LDM_HASHRATELOG_MAX), opaqueAPI) );
                    if (FUZ_rand(&lseed) & 3) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_srcSizeHint, FUZ_randomClampedLength(&lseed, ZSTD_SRCSIZEHINT_MIN, ZSTD_SRCSIZEHINT_MAX), opaqueAPI) );
                }

                /* mess with frame parameters */
                if (FUZ_rand(&lseed) & 1) {
                    int const checksumFlag = FUZ_rand(&lseed) & 1;
                    DISPLAYLEVEL(5, "t%u: frame checksum : %u \n", testNb, checksumFlag);
                    CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_checksumFlag, checksumFlag, opaqueAPI) );
                }
                if (FUZ_rand(&lseed) & 1) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_dictIDFlag, FUZ_rand(&lseed) & 1, opaqueAPI) );
                if (FUZ_rand(&lseed) & 1) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_contentSizeFlag, FUZ_rand(&lseed) & 1, opaqueAPI) );
                if (FUZ_rand(&lseed) & 1) {
                    DISPLAYLEVEL(5, "t%u: pledgedSrcSize : %u \n", testNb, (unsigned)pledgedSrcSize);
                    CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, pledgedSrcSize) );
                } else {
                    pledgedSrcSize = ZSTD_CONTENTSIZE_UNKNOWN;
                }

                /* multi-threading parameters. Only adjust occasionally for small tests. */
                if (bigTests || (FUZ_rand(&lseed) & 0xF) == 0xF) {
                    U32 const nbThreadsCandidate = (FUZ_rand(&lseed) & 4) + 1;
                    U32 const nbThreadsAdjusted = (windowLogMalus < nbThreadsCandidate) ? nbThreadsCandidate - windowLogMalus : 1;
                    int const nbThreads = MIN(nbThreadsAdjusted, nbThreadsMax);
                    DISPLAYLEVEL(5, "t%i: nbThreads : %u \n", testNb, nbThreads);
                    CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_nbWorkers, nbThreads, opaqueAPI) );
                    if (nbThreads > 1) {
                        U32 const jobLog = FUZ_rand(&lseed) % (testLog+1);
                        CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_overlapLog, FUZ_rand(&lseed) % 10, opaqueAPI) );
                        CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_jobSize, (U32)FUZ_rLogLength(&lseed, jobLog), opaqueAPI) );
                    }
                }
                /* Enable rsyncable mode 1 in 4 times. */
                {
                    int const rsyncable = (FUZ_rand(&lseed) % 4 == 0);
                    DISPLAYLEVEL(5, "t%u: rsyncable : %d \n", testNb, rsyncable);
                    setCCtxParameter(zc, cctxParams, ZSTD_c_rsyncable, rsyncable, opaqueAPI);
                }

                if (FUZ_rand(&lseed) & 1) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_forceMaxWindow, FUZ_rand(&lseed) & 1, opaqueAPI) );
                if (FUZ_rand(&lseed) & 1) CHECK_Z( setCCtxParameter(zc, cctxParams, ZSTD_c_deterministicRefPrefix, FUZ_rand(&lseed) & 1, opaqueAPI) );

                /* Apply parameters */
                if (opaqueAPI) {
                    DISPLAYLEVEL(5, "t%u: applying CCtxParams \n", testNb);
                    CHECK_Z (ZSTD_CCtx_setParametersUsingCCtxParams(zc, cctxParams) );
                }

                if (FUZ_rand(&lseed) & 1) {
                    if (FUZ_rand(&lseed) & 1) {
                        CHECK_Z( ZSTD_CCtx_loadDictionary(zc, dict, dictSize) );
                    } else {
                        CHECK_Z( ZSTD_CCtx_loadDictionary_byReference(zc, dict, dictSize) );
                    }
                } else {
                    isRefPrefix = 1;
                    CHECK_Z( ZSTD_CCtx_refPrefix(zc, dict, dictSize) );
                }
        }   }

        CHECK_Z(getCCtxParams(zc, &savedParams));

        /* multi-segments compression test */
        {   int iter;
            int const startSeed = lseed;
            XXH64_hash_t compressedCrcs[2];
            for (iter = 0; iter < 2; ++iter, lseed = startSeed) {
                ZSTD_outBuffer outBuff = { cBuffer, cBufferSize, 0 } ;
                int const singlePass = (FUZ_rand(&lseed) & 3) == 0;
                int nbWorkers;

                XXH64_reset(&xxhState, 0);

                CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(zc, pledgedSrcSize) );
                if (isRefPrefix) {
                    DISPLAYLEVEL(6, "t%u: Reloading prefix\n", testNb);
                    /* Need to reload the prefix because it gets dropped after one compression */
                    CHECK_Z( ZSTD_CCtx_refPrefix(zc, dict, dictSize) );
                }

                /* Adjust number of workers occasionally - result must be deterministic independent of nbWorkers */
                CHECK_Z(ZSTD_CCtx_getParameter(zc, ZSTD_c_nbWorkers, &nbWorkers));
                if (nbWorkers > 0 && (FUZ_rand(&lseed) & 7) == 0) {
                    DISPLAYLEVEL(6, "t%u: Modify nbWorkers: %d -> %d \n", testNb, nbWorkers, nbWorkers + iter);
                    CHECK_Z(ZSTD_CCtx_setParameter(zc, ZSTD_c_nbWorkers, nbWorkers + iter));
                }

                if (singlePass) {
                    ZSTD_inBuffer inBuff = { srcBuffer, maxTestSize, 0 };
                    CHECK_Z(ZSTD_compressStream2(zc, &outBuff, &inBuff, ZSTD_e_end));
                    DISPLAYLEVEL(6, "t%u: Single pass compression: consumed %u bytes ; produced %u bytes \n",
                        testNb, (unsigned)inBuff.pos, (unsigned)outBuff.pos);
                    CHECK(inBuff.pos != inBuff.size, "Input not consumed!");
                    crcOrig = XXH64(srcBuffer, maxTestSize, 0);
                    totalTestSize = maxTestSize;
                } else {
                    outBuff.size = 0;
                    for (totalTestSize=0 ; (totalTestSize < maxTestSize) ; ) {
                        /* compress random chunks into randomly sized dst buffers */
                        size_t const randomSrcSize = FUZ_randomLength(&lseed, maxSampleLog);
                        size_t const srcSize = MIN(maxTestSize-totalTestSize, randomSrcSize);
                        size_t const srcStart = FUZ_rand(&lseed) % (srcBufferSize - srcSize);
                        ZSTD_EndDirective const flush = (FUZ_rand(&lseed) & 15) ? ZSTD_e_continue : ZSTD_e_flush;
                        ZSTD_inBuffer inBuff = { srcBuffer+srcStart, srcSize, 0 };
                        int forwardProgress;
                        do {
                            size_t const ipos = inBuff.pos;
                            size_t const opos = outBuff.pos;
                            size_t ret;
                            if (outBuff.pos == outBuff.size) {
                                size_t const randomDstSize = FUZ_randomLength(&lseed, maxSampleLog+1);
                                size_t const dstBuffSize = MIN(cBufferSize - outBuff.pos, randomDstSize);
                                outBuff.size = outBuff.pos + dstBuffSize;
                            }
                            CHECK_Z( ret = ZSTD_compressStream2(zc, &outBuff, &inBuff, flush) );
                            DISPLAYLEVEL(6, "t%u: compress consumed %u bytes (total : %u) ; flush: %u (total : %u) \n",
                                testNb, (unsigned)inBuff.pos, (unsigned)(totalTestSize + inBuff.pos), (unsigned)flush, (unsigned)outBuff.pos);

                            /* We've completed the flush */
                            if (flush == ZSTD_e_flush && ret == 0)
                                break;

                            /* Ensure maximal forward progress for determinism */
                            forwardProgress = (inBuff.pos != ipos) || (outBuff.pos != opos);
                        } while (forwardProgress);
                        assert(inBuff.pos == inBuff.size);

                        XXH64_update(&xxhState, srcBuffer+srcStart, inBuff.pos);
                        memcpy(copyBuffer+totalTestSize, srcBuffer+srcStart, inBuff.pos);
                        totalTestSize += inBuff.pos;
                    }

                    /* final frame epilogue */
                    {   size_t remainingToFlush = 1;
                        while (remainingToFlush) {
                            ZSTD_inBuffer inBuff = { NULL, 0, 0 };
                            size_t const randomDstSize = FUZ_randomLength(&lseed, maxSampleLog+1);
                            size_t const adjustedDstSize = MIN(cBufferSize - outBuff.pos, randomDstSize);
                            outBuff.size = outBuff.pos + adjustedDstSize;
                            DISPLAYLEVEL(6, "t%u: End-flush into dst buffer of size %u \n", testNb, (unsigned)adjustedDstSize);
                            /* ZSTD_e_end guarantees maximal forward progress */
                            remainingToFlush = ZSTD_compressStream2(zc, &outBuff, &inBuff, ZSTD_e_end);
                            DISPLAYLEVEL(6, "t%u: Total flushed so far : %u bytes \n", testNb, (unsigned)outBuff.pos);
                            CHECK( ZSTD_isError(remainingToFlush),
                                "ZSTD_compressStream2 w/ ZSTD_e_end error : %s",
                                ZSTD_getErrorName(remainingToFlush) );
                    }   }
                    crcOrig = XXH64_digest(&xxhState);
                }
                cSize = outBuff.pos;
                compressedCrcs[iter] = XXH64(cBuffer, cSize, 0);
                DISPLAYLEVEL(5, "Frame completed : %zu bytes \n", cSize);
            }
            CHECK(!(compressedCrcs[0] == compressedCrcs[1]), "Compression is not deterministic!");
        }

        CHECK(badParameters(zc, savedParams), "CCtx params are wrong");

        /* multi - fragments decompression test */
        if (FUZ_rand(&lseed) & 1) {
            CHECK_Z(ZSTD_DCtx_reset(zd, ZSTD_reset_session_and_parameters));
        }
        if (!dictSize /* don't reset if dictionary : could be different */ && (FUZ_rand(&lseed) & 1)) {
            DISPLAYLEVEL(5, "resetting DCtx (dict:%p) \n", (void const*)dict);
            CHECK_Z( ZSTD_resetDStream(zd) );
        } else {
            if (dictSize)
                DISPLAYLEVEL(5, "using dictionary of size %zu \n", dictSize);
            CHECK_Z( ZSTD_initDStream_usingDict(zd, dict, dictSize) );
        }
        if (FUZ_rand(&lseed) & 1) {
            CHECK_Z(ZSTD_DCtx_setParameter(zd, ZSTD_d_disableHuffmanAssembly, FUZ_rand(&lseed) & 1));
        }
        {   size_t decompressionResult = 1;
            ZSTD_inBuffer  inBuff = { cBuffer, cSize, 0 };
            ZSTD_outBuffer outBuff= { dstBuffer, dstBufferSize, 0 };
            for (totalGenSize = 0 ; decompressionResult ; ) {
                size_t const readCSrcSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const randomDstSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const dstBuffSize = MIN(dstBufferSize - totalGenSize, randomDstSize);
                inBuff.size = inBuff.pos + readCSrcSize;
                outBuff.size = outBuff.pos + dstBuffSize;
                DISPLAYLEVEL(6, "decompression presented %u new bytes (pos:%u/%u)\n",
                                (unsigned)readCSrcSize, (unsigned)inBuff.pos, (unsigned)cSize);
                decompressionResult = ZSTD_decompressStream(zd, &outBuff, &inBuff);
                DISPLAYLEVEL(6, "so far: consumed = %u, produced = %u \n",
                                (unsigned)inBuff.pos, (unsigned)outBuff.pos);
                if (ZSTD_isError(decompressionResult)) {
                    DISPLAY("ZSTD_decompressStream error : %s \n", ZSTD_getErrorName(decompressionResult));
                    findDiff(copyBuffer, dstBuffer, totalTestSize);
                }
                CHECK (ZSTD_isError(decompressionResult), "decompression error : %s", ZSTD_getErrorName(decompressionResult));
                CHECK (inBuff.pos > cSize, "ZSTD_decompressStream consumes too much input : %u > %u ", (unsigned)inBuff.pos, (unsigned)cSize);
            }
            CHECK (inBuff.pos != cSize, "compressed data should be fully read (%u != %u)", (unsigned)inBuff.pos, (unsigned)cSize);
            CHECK (outBuff.pos != totalTestSize, "decompressed data : wrong size (%u != %u)", (unsigned)outBuff.pos, (unsigned)totalTestSize);
            {   U64 const crcDest = XXH64(dstBuffer, totalTestSize, 0);
                if (crcDest!=crcOrig) findDiff(copyBuffer, dstBuffer, totalTestSize);
                CHECK (crcDest!=crcOrig, "decompressed data corrupted");
        }   }

        /*=====   noisy/erroneous src decompression test   =====*/

        /* add some noise */
        {   U32 const nbNoiseChunks = (FUZ_rand(&lseed) & 7) + 2;
            U32 nn; for (nn=0; nn<nbNoiseChunks; nn++) {
                size_t const randomNoiseSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const noiseSize  = MIN((cSize/3) , randomNoiseSize);
                size_t const noiseStart = FUZ_rand(&lseed) % (srcBufferSize - noiseSize);
                size_t const cStart = FUZ_rand(&lseed) % (cSize - noiseSize);
                memcpy(cBuffer+cStart, srcBuffer+noiseStart, noiseSize);
        }   }

        /* try decompression on noisy data */
        if (FUZ_rand(&lseed) & 1) {
            CHECK_Z(ZSTD_DCtx_reset(zd_noise, ZSTD_reset_session_and_parameters));
        } else {
            CHECK_Z(ZSTD_DCtx_reset(zd_noise, ZSTD_reset_session_only));
        }
        if (FUZ_rand(&lseed) & 1) {
            CHECK_Z(ZSTD_DCtx_setParameter(zd_noise, ZSTD_d_disableHuffmanAssembly, FUZ_rand(&lseed) & 1));
        }
        {   ZSTD_inBuffer  inBuff = { cBuffer, cSize, 0 };
            ZSTD_outBuffer outBuff= { dstBuffer, dstBufferSize, 0 };
            while (outBuff.pos < dstBufferSize) {
                size_t const randomCSrcSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const randomDstSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const adjustedDstSize = MIN(dstBufferSize - outBuff.pos, randomDstSize);
                size_t const adjustedCSrcSize = MIN(cSize - inBuff.pos, randomCSrcSize);
                outBuff.size = outBuff.pos + adjustedDstSize;
                inBuff.size  = inBuff.pos + adjustedCSrcSize;
                {   size_t const decompressError = ZSTD_decompressStream(zd, &outBuff, &inBuff);
                    if (ZSTD_isError(decompressError)) break;   /* error correctly detected */
                    /* Good so far, but no more progress possible */
                    if (outBuff.pos < outBuff.size && inBuff.pos == cSize) break;
    }   }   }   }
    DISPLAY("\r%u fuzzer tests completed   \n", testNb-1);

_cleanup:
    ZSTD_freeCCtx(zc);
    ZSTD_freeDStream(zd);
    ZSTD_freeDStream(zd_noise);
    ZSTD_freeCCtxParams(cctxParams);
    free(cNoiseBuffer[0]);
    free(cNoiseBuffer[1]);
    free(cNoiseBuffer[2]);
    free(cNoiseBuffer[3]);
    free(cNoiseBuffer[4]);
    free(copyBuffer);
    free(cBuffer);
    free(dstBuffer);
    return result;

_output_error:
    result = 1;
    goto _cleanup;
}

/*-*******************************************************
*  Command line
*********************************************************/
static int FUZ_usage(const char* programName)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [args]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Arguments :\n");
    DISPLAY( " -i#    : Number of tests (default:%u)\n", nbTestsDefault);
    DISPLAY( " -T#    : Max duration to run for. Overrides number of tests. (e.g. -T1m or -T60s for one minute)\n");
    DISPLAY( " -s#    : Select seed (default:prompt user)\n");
    DISPLAY( " -t#    : Select starting test number (default:0)\n");
    DISPLAY( " -P#    : Select compressibility in %% (default:%i%%)\n", FUZ_COMPRESSIBILITY_DEFAULT);
    DISPLAY( " -v     : verbose\n");
    DISPLAY( " -p     : pause at the end\n");
    DISPLAY( " -h     : display help and exit\n");
    return 0;
}

typedef enum { simple_api, advanced_api } e_api;

int main(int argc, const char** argv)
{
    U32 seed = 0;
    int seedset = 0;
    int nbTests = nbTestsDefault;
    int testNb = 0;
    int proba = FUZ_COMPRESSIBILITY_DEFAULT;
    int result = 0;
    int mainPause = 0;
    int bigTests = (sizeof(size_t) == 8);
    e_api selected_api = simple_api;
    const char* const programName = argv[0];
    int argNb;

    /* Check command line */
    for(argNb=1; argNb<argc; argNb++) {
        const char* argument = argv[argNb];
        assert(argument != NULL);

        /* Parsing commands. Aggregated commands are allowed */
        if (argument[0]=='-') {

            if (!strcmp(argument, "--newapi")) { selected_api=advanced_api; testNb += !testNb; continue; }
            if (!strcmp(argument, "--no-big-tests")) { bigTests=0; continue; }
            if (!strcmp(argument, "--big-tests")) { bigTests=1; continue; }

            argument++;
            while (*argument!=0) {
                switch(*argument)
                {
                case 'h':
                    return FUZ_usage(programName);

                case 'v':
                    argument++;
                    g_displayLevel++;
                    break;

                case 'q':
                    argument++;
                    g_displayLevel--;
                    break;

                case 'p': /* pause at the end */
                    argument++;
                    mainPause = 1;
                    break;

                case 'i':   /* limit tests by nb of iterations (default) */
                    argument++;
                    nbTests=0; g_clockTime=0;
                    while ((*argument>='0') && (*argument<='9')) {
                        nbTests *= 10;
                        nbTests += *argument - '0';
                        argument++;
                    }
                    break;

                case 'T':   /* limit tests by time */
                    argument++;
                    nbTests=0; g_clockTime=0;
                    while ((*argument>='0') && (*argument<='9')) {
                        g_clockTime *= 10;
                        g_clockTime += *argument - '0';
                        argument++;
                    }
                    if (*argument=='m') {    /* -T1m == -T60 */
                        g_clockTime *=60, argument++;
                        if (*argument=='n') argument++; /* -T1mn == -T60 */
                    } else if (*argument=='s') argument++; /* -T10s == -T10 */
                    g_clockTime *= SEC_TO_MICRO;
                    break;

                case 's':   /* manually select seed */
                    argument++;
                    seedset=1;
                    seed=0;
                    while ((*argument>='0') && (*argument<='9')) {
                        seed *= 10;
                        seed += *argument - '0';
                        argument++;
                    }
                    break;

                case 't':   /* select starting test number */
                    argument++;
                    testNb=0;
                    while ((*argument>='0') && (*argument<='9')) {
                        testNb *= 10;
                        testNb += *argument - '0';
                        argument++;
                    }
                    break;

                case 'P':   /* compressibility % */
                    argument++;
                    proba=0;
                    while ((*argument>='0') && (*argument<='9')) {
                        proba *= 10;
                        proba += *argument - '0';
                        argument++;
                    }
                    if (proba<0) proba=0;
                    if (proba>100) proba=100;
                    break;

                default:
                    return FUZ_usage(programName);
                }
    }   }   }   /* for(argNb=1; argNb<argc; argNb++) */

    /* Get Seed */
    DISPLAY("Starting zstream tester (%i-bits, %s)\n", (int)(sizeof(size_t)*8), ZSTD_VERSION_STRING);

    if (!seedset) {
        time_t const t = time(NULL);
        U32 const h = XXH32(&t, sizeof(t), 1);
        seed = h % 10000;
    }

    DISPLAY("Seed = %u\n", (unsigned)seed);
    if (proba!=FUZ_COMPRESSIBILITY_DEFAULT) DISPLAY("Compressibility : %i%%\n", proba);

    if (nbTests<=0) nbTests=1;

    if (testNb==0) {
        result = basicUnitTests(0, ((double)proba) / 100, bigTests);  /* constant seed for predictability */
    }

    if (!result) {
        switch(selected_api)
        {
        case simple_api :
            result = fuzzerTests(seed, nbTests, testNb, ((double)proba) / 100, bigTests);
            break;
        case advanced_api :
            result = fuzzerTests_newAPI(seed, nbTests, testNb, ((double)proba) / 100, bigTests);
            break;
        default :
            assert(0);   /* impossible */
        }
    }

    if (mainPause) {
        int unused;
        DISPLAY("Press Enter \n");
        unused = getchar();
        (void)unused;
    }
    return result;
}
