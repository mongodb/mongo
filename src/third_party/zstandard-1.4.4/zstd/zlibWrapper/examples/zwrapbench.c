/*
 * Copyright (c) 2016-present, Przemyslaw Skibinski, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 */


/* *************************************
*  Includes
***************************************/
#include "util.h"        /* Compiler options, UTIL_GetFileSize, UTIL_sleep */
#include <stdlib.h>      /* malloc, free */
#include <string.h>      /* memset */
#include <stdio.h>       /* fprintf, fopen, ftello64 */
#include <time.h>        /* clock_t, clock, CLOCKS_PER_SEC */
#include <ctype.h>       /* toupper */
#include <errno.h>       /* errno */

#include "timefn.h"      /* UTIL_time_t, UTIL_getTime, UTIL_clockSpanMicro, UTIL_waitForNextTick */
#include "mem.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "datagen.h"     /* RDG_genBuffer */
#include "xxhash.h"

#include "zstd_zlibwrapper.h"



/*-************************************
*  Tuning parameters
**************************************/
#ifndef ZSTDCLI_CLEVEL_DEFAULT
#  define ZSTDCLI_CLEVEL_DEFAULT 3
#endif


/*-************************************
*  Constants
**************************************/
#define COMPRESSOR_NAME "Zstandard wrapper for zlib command line interface"
#ifndef ZSTD_VERSION
#  define ZSTD_VERSION "v" ZSTD_VERSION_STRING
#endif
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s %i-bits %s, by %s ***\n", COMPRESSOR_NAME, (int)(sizeof(size_t)*8), ZSTD_VERSION, AUTHOR

#ifndef ZSTD_GIT_COMMIT
#  define ZSTD_GIT_COMMIT_STRING ""
#else
#  define ZSTD_GIT_COMMIT_STRING ZSTD_EXPAND_AND_QUOTE(ZSTD_GIT_COMMIT)
#endif

#define NBLOOPS               3
#define TIMELOOP_MICROSEC     1*1000000ULL /* 1 second */
#define ACTIVEPERIOD_MICROSEC 70*1000000ULL /* 70 seconds */
#define COOLPERIOD_SEC        10

#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)

static const size_t maxMemory = (sizeof(size_t)==4)  ?  (2 GB - 64 MB) : (size_t)(1ULL << ((sizeof(size_t)*8)-31));

static U32 g_compressibilityDefault = 50;


/* *************************************
*  console display
***************************************/
#define DEFAULT_DISPLAY_LEVEL 2
#define DISPLAY(...)         fprintf(displayOut, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); }
static int g_displayLevel = DEFAULT_DISPLAY_LEVEL;   /* 0 : no display;   1: errors;   2 : + result + interaction + warnings;   3 : + progression;   4 : + information */
static FILE* displayOut;

#define DISPLAYUPDATE(l, ...) if (g_displayLevel>=l) { \
            if ((clock() - g_time > refreshRate) || (g_displayLevel>=4)) \
            { g_time = clock(); DISPLAY(__VA_ARGS__); \
            if (g_displayLevel>=4) fflush(displayOut); } }
static const clock_t refreshRate = CLOCKS_PER_SEC * 15 / 100;
static clock_t g_time = 0;


/* *************************************
*  Exceptions
***************************************/
#ifndef DEBUG
#  define DEBUG 0
#endif
#define DEBUGOUTPUT(...) { if (DEBUG) DISPLAY(__VA_ARGS__); }
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "Error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, "\n");                                                \
    exit(error);                                                          \
}


/* *************************************
*  Benchmark Parameters
***************************************/
static unsigned g_nbIterations = NBLOOPS;
static size_t g_blockSize = 0;
int g_additionalParam = 0;

void BMK_setNotificationLevel(unsigned level) { g_displayLevel=level; }

void BMK_setAdditionalParam(int additionalParam) { g_additionalParam=additionalParam; }

void BMK_SetNbIterations(unsigned nbLoops)
{
    g_nbIterations = nbLoops;
    DISPLAYLEVEL(3, "- test >= %u seconds per compression / decompression -\n", g_nbIterations);
}

void BMK_SetBlockSize(size_t blockSize)
{
    g_blockSize = blockSize;
    DISPLAYLEVEL(2, "using blocks of size %u KB \n", (unsigned)(blockSize>>10));
}


/* ********************************************************
*  Bench functions
**********************************************************/
#undef MIN
#undef MAX
#define MIN(a,b) ((a)<(b) ? (a) : (b))
#define MAX(a,b) ((a)>(b) ? (a) : (b))

typedef struct
{
    z_const char* srcPtr;
    size_t srcSize;
    char*  cPtr;
    size_t cRoom;
    size_t cSize;
    char*  resPtr;
    size_t resSize;
} blockParam_t;

typedef enum { BMK_ZSTD, BMK_ZSTD_STREAM, BMK_ZLIB, BMK_ZWRAP_ZLIB, BMK_ZWRAP_ZSTD, BMK_ZLIB_REUSE, BMK_ZWRAP_ZLIB_REUSE, BMK_ZWRAP_ZSTD_REUSE } BMK_compressor;


static int BMK_benchMem(z_const void* srcBuffer, size_t srcSize,
                        const char* displayName, int cLevel,
                        const size_t* fileSizes, U32 nbFiles,
                        const void* dictBuffer, size_t dictBufferSize, BMK_compressor compressor)
{
    size_t const blockSize = (g_blockSize>=32 ? g_blockSize : srcSize) + (!srcSize) /* avoid div by 0 */ ;
    size_t const avgSize = MIN(g_blockSize, (srcSize / nbFiles));
    U32 const maxNbBlocks = (U32) ((srcSize + (blockSize-1)) / blockSize) + nbFiles;
    blockParam_t* const blockTable = (blockParam_t*) malloc(maxNbBlocks * sizeof(blockParam_t));
    size_t const maxCompressedSize = ZSTD_compressBound(srcSize) + (maxNbBlocks * 1024);   /* add some room for safety */
    void* const compressedBuffer = malloc(maxCompressedSize);
    void* const resultBuffer = malloc(srcSize);
    ZSTD_CCtx* const ctx = ZSTD_createCCtx();
    ZSTD_DCtx* const dctx = ZSTD_createDCtx();
    U32 nbBlocks;

    /* checks */
    if (!compressedBuffer || !resultBuffer || !blockTable || !ctx || !dctx)
        EXM_THROW(31, "allocation error : not enough memory");

    /* init */
    if (strlen(displayName)>17) displayName += strlen(displayName)-17;   /* can only display 17 characters */

    /* Init blockTable data */
    {   z_const char* srcPtr = (z_const char*)srcBuffer;
        char* cPtr = (char*)compressedBuffer;
        char* resPtr = (char*)resultBuffer;
        U32 fileNb;
        for (nbBlocks=0, fileNb=0; fileNb<nbFiles; fileNb++) {
            size_t remaining = fileSizes[fileNb];
            U32 const nbBlocksforThisFile = (U32)((remaining + (blockSize-1)) / blockSize);
            U32 const blockEnd = nbBlocks + nbBlocksforThisFile;
            for ( ; nbBlocks<blockEnd; nbBlocks++) {
                size_t const thisBlockSize = MIN(remaining, blockSize);
                blockTable[nbBlocks].srcPtr = srcPtr;
                blockTable[nbBlocks].cPtr = cPtr;
                blockTable[nbBlocks].resPtr = resPtr;
                blockTable[nbBlocks].srcSize = thisBlockSize;
                blockTable[nbBlocks].cRoom = ZSTD_compressBound(thisBlockSize);
                srcPtr += thisBlockSize;
                cPtr += blockTable[nbBlocks].cRoom;
                resPtr += thisBlockSize;
                remaining -= thisBlockSize;
    }   }   }

    /* warming up memory */
    RDG_genBuffer(compressedBuffer, maxCompressedSize, 0.10, 0.50, 1);

    /* Bench */
    {   U64 fastestC = (U64)(-1LL), fastestD = (U64)(-1LL);
        U64 const crcOrig = XXH64(srcBuffer, srcSize, 0);
        UTIL_time_t coolTime;
        U64 const maxTime = (g_nbIterations * TIMELOOP_MICROSEC) + 100;
        U64 totalCTime=0, totalDTime=0;
        U32 cCompleted=0, dCompleted=0;
#       define NB_MARKS 4
        const char* const marks[NB_MARKS] = { " |", " /", " =",  "\\" };
        U32 markNb = 0;
        size_t cSize = 0;
        double ratio = 0.;

        coolTime = UTIL_getTime();
        DISPLAYLEVEL(2, "\r%79s\r", "");
        while (!cCompleted | !dCompleted) {
            UTIL_time_t clockStart;
            U64 clockLoop = g_nbIterations ? TIMELOOP_MICROSEC : 1;

            /* overheat protection */
            if (UTIL_clockSpanMicro(coolTime) > ACTIVEPERIOD_MICROSEC) {
                DISPLAYLEVEL(2, "\rcooling down ...    \r");
                UTIL_sleep(COOLPERIOD_SEC);
                coolTime = UTIL_getTime();
            }

            /* Compression */
            DISPLAYLEVEL(2, "%2s-%-17.17s :%10u ->\r", marks[markNb], displayName, (unsigned)srcSize);
            if (!cCompleted) memset(compressedBuffer, 0xE5, maxCompressedSize);  /* warm up and erase result buffer */

            UTIL_sleepMilli(1);  /* give processor time to other processes */
            UTIL_waitForNextTick();
            clockStart = UTIL_getTime();

            if (!cCompleted) {   /* still some time to do compression tests */
                U32 nbLoops = 0;
                if (compressor == BMK_ZSTD) {
                    ZSTD_parameters const zparams = ZSTD_getParams(cLevel, avgSize, dictBufferSize);
                    ZSTD_customMem const cmem = { NULL, NULL, NULL };
                    ZSTD_CDict* const cdict = ZSTD_createCDict_advanced(dictBuffer, dictBufferSize, ZSTD_dlm_byRef, ZSTD_dct_auto, zparams.cParams, cmem);
                    if (cdict==NULL) EXM_THROW(1, "ZSTD_createCDict_advanced() allocation failure");

                    do {
                        U32 blockNb;
                        size_t rSize;
                        for (blockNb=0; blockNb<nbBlocks; blockNb++) {
                            if (dictBufferSize) {
                                rSize = ZSTD_compress_usingCDict(ctx,
                                                blockTable[blockNb].cPtr,  blockTable[blockNb].cRoom,
                                                blockTable[blockNb].srcPtr,blockTable[blockNb].srcSize,
                                                cdict);
                            } else {
                                rSize = ZSTD_compressCCtx (ctx,
                                                blockTable[blockNb].cPtr,  blockTable[blockNb].cRoom,
                                                blockTable[blockNb].srcPtr,blockTable[blockNb].srcSize, cLevel);
                            }
                            if (ZSTD_isError(rSize)) EXM_THROW(1, "ZSTD_compress_usingCDict() failed : %s", ZSTD_getErrorName(rSize));
                            blockTable[blockNb].cSize = rSize;
                        }
                        nbLoops++;
                    } while (UTIL_clockSpanMicro(clockStart) < clockLoop);
                    ZSTD_freeCDict(cdict);
                } else if (compressor == BMK_ZSTD_STREAM) {
                    ZSTD_parameters const zparams = ZSTD_getParams(cLevel, avgSize, dictBufferSize);
                    ZSTD_inBuffer inBuffer;
                    ZSTD_outBuffer outBuffer;
                    ZSTD_CStream* zbc = ZSTD_createCStream();
                    size_t rSize;
                    if (zbc == NULL) EXM_THROW(1, "ZSTD_createCStream() allocation failure");
                    rSize = ZSTD_initCStream_advanced(zbc, dictBuffer, dictBufferSize, zparams, avgSize);
                    if (ZSTD_isError(rSize)) EXM_THROW(1, "ZSTD_initCStream_advanced() failed : %s", ZSTD_getErrorName(rSize));
                    do {
                        U32 blockNb;
                        for (blockNb=0; blockNb<nbBlocks; blockNb++) {
                            rSize = ZSTD_resetCStream(zbc, blockTable[blockNb].srcSize);
                            if (ZSTD_isError(rSize)) EXM_THROW(1, "ZSTD_resetCStream() failed : %s", ZSTD_getErrorName(rSize));
                            inBuffer.src = blockTable[blockNb].srcPtr;
                            inBuffer.size = blockTable[blockNb].srcSize;
                            inBuffer.pos = 0;
                            outBuffer.dst = blockTable[blockNb].cPtr;
                            outBuffer.size = blockTable[blockNb].cRoom;
                            outBuffer.pos = 0;
                            rSize = ZSTD_compressStream(zbc, &outBuffer, &inBuffer);
                            if (ZSTD_isError(rSize)) EXM_THROW(1, "ZSTD_compressStream() failed : %s", ZSTD_getErrorName(rSize));
                            rSize = ZSTD_endStream(zbc, &outBuffer);
                            if (ZSTD_isError(rSize)) EXM_THROW(1, "ZSTD_endStream() failed : %s", ZSTD_getErrorName(rSize));
                            blockTable[blockNb].cSize = outBuffer.pos;
                        }
                        nbLoops++;
                    } while (UTIL_clockSpanMicro(clockStart) < clockLoop);
                    ZSTD_freeCStream(zbc);
                } else if (compressor == BMK_ZWRAP_ZLIB_REUSE || compressor == BMK_ZWRAP_ZSTD_REUSE || compressor == BMK_ZLIB_REUSE) {
                    z_stream def;
                    int ret;
                    int useSetDict = (dictBuffer != NULL);
                    if (compressor == BMK_ZLIB_REUSE || compressor == BMK_ZWRAP_ZLIB_REUSE) ZWRAP_useZSTDcompression(0);
                    else ZWRAP_useZSTDcompression(1);
                    def.zalloc = Z_NULL;
                    def.zfree = Z_NULL;
                    def.opaque = Z_NULL;
                    ret = deflateInit(&def, cLevel);
                    if (ret != Z_OK) EXM_THROW(1, "deflateInit failure");
                 /*   if (ZWRAP_isUsingZSTDcompression()) {
                        ret = ZWRAP_setPledgedSrcSize(&def, avgSize);
                        if (ret != Z_OK) EXM_THROW(1, "ZWRAP_setPledgedSrcSize failure");
                    } */
                    do {
                        U32 blockNb;
                        for (blockNb=0; blockNb<nbBlocks; blockNb++) {
                            if (ZWRAP_isUsingZSTDcompression())
                                ret = ZWRAP_deflateReset_keepDict(&def); /* reuse dictionary to make compression faster */
                            else
                                ret = deflateReset(&def);
                            if (ret != Z_OK) EXM_THROW(1, "deflateReset failure");
                            if (useSetDict) {
                                ret = deflateSetDictionary(&def, (const z_Bytef*)dictBuffer, dictBufferSize);
                                if (ret != Z_OK) EXM_THROW(1, "deflateSetDictionary failure");
                                if (ZWRAP_isUsingZSTDcompression()) useSetDict = 0; /* zstd doesn't require deflateSetDictionary after ZWRAP_deflateReset_keepDict */
                            }
                            def.next_in = (z_const z_Bytef*) blockTable[blockNb].srcPtr;
                            def.avail_in = (uInt)blockTable[blockNb].srcSize;
                            def.total_in = 0;
                            def.next_out = (z_Bytef*) blockTable[blockNb].cPtr;
                            def.avail_out = (uInt)blockTable[blockNb].cRoom;
                            def.total_out = 0;
                            ret = deflate(&def, Z_FINISH);
                            if (ret != Z_STREAM_END) EXM_THROW(1, "deflate failure ret=%d srcSize=%d" , ret, (int)blockTable[blockNb].srcSize);
                            blockTable[blockNb].cSize = def.total_out;
                        }
                        nbLoops++;
                    } while (UTIL_clockSpanMicro(clockStart) < clockLoop);
                    ret = deflateEnd(&def);
                    if (ret != Z_OK) EXM_THROW(1, "deflateEnd failure");
                } else {
                    z_stream def;
                    if (compressor == BMK_ZLIB || compressor == BMK_ZWRAP_ZLIB) ZWRAP_useZSTDcompression(0);
                    else ZWRAP_useZSTDcompression(1);
                    do {
                        U32 blockNb;
                        for (blockNb=0; blockNb<nbBlocks; blockNb++) {
                            int ret;
                            def.zalloc = Z_NULL;
                            def.zfree = Z_NULL;
                            def.opaque = Z_NULL;
                            ret = deflateInit(&def, cLevel);
                            if (ret != Z_OK) EXM_THROW(1, "deflateInit failure");
                            if (dictBuffer) {
                                ret = deflateSetDictionary(&def, (const z_Bytef*)dictBuffer, dictBufferSize);
                                if (ret != Z_OK) EXM_THROW(1, "deflateSetDictionary failure");
                            }
                            def.next_in = (z_const z_Bytef*) blockTable[blockNb].srcPtr;
                            def.avail_in = (uInt)blockTable[blockNb].srcSize;
                            def.total_in = 0;
                            def.next_out = (z_Bytef*) blockTable[blockNb].cPtr;
                            def.avail_out = (uInt)blockTable[blockNb].cRoom;
                            def.total_out = 0;
                            ret = deflate(&def, Z_FINISH);
                            if (ret != Z_STREAM_END) EXM_THROW(1, "deflate failure");
                            ret = deflateEnd(&def);
                            if (ret != Z_OK) EXM_THROW(1, "deflateEnd failure");
                            blockTable[blockNb].cSize = def.total_out;
                        }
                        nbLoops++;
                    } while (UTIL_clockSpanMicro(clockStart) < clockLoop);
                }
                {   U64 const clockSpan = UTIL_clockSpanMicro(clockStart);
                    if (clockSpan < fastestC*nbLoops) fastestC = clockSpan / nbLoops;
                    totalCTime += clockSpan;
                    cCompleted = totalCTime>maxTime;
            }   }

            cSize = 0;
            { U32 blockNb; for (blockNb=0; blockNb<nbBlocks; blockNb++) cSize += blockTable[blockNb].cSize; }
            ratio = (double)srcSize / (double)cSize;
            markNb = (markNb+1) % NB_MARKS;
            DISPLAYLEVEL(2, "%2s-%-17.17s :%10u ->%10u (%5.3f),%6.1f MB/s\r",
                    marks[markNb], displayName, (unsigned)srcSize, (unsigned)cSize, ratio,
                    (double)srcSize / fastestC );

            (void)fastestD; (void)crcOrig;   /*  unused when decompression disabled */
#if 1
            /* Decompression */
            if (!dCompleted) memset(resultBuffer, 0xD6, srcSize);  /* warm result buffer */

            UTIL_sleepMilli(1); /* give processor time to other processes */
            UTIL_waitForNextTick();
            clockStart = UTIL_getTime();

            if (!dCompleted) {
                U32 nbLoops = 0;
                if (compressor == BMK_ZSTD) {
                    ZSTD_DDict* ddict = ZSTD_createDDict(dictBuffer, dictBufferSize);
                    if (!ddict) EXM_THROW(2, "ZSTD_createDDict() allocation failure");
                    do {
                        unsigned blockNb;
                        for (blockNb=0; blockNb<nbBlocks; blockNb++) {
                            size_t const regenSize = ZSTD_decompress_usingDDict(dctx,
                                blockTable[blockNb].resPtr, blockTable[blockNb].srcSize,
                                blockTable[blockNb].cPtr, blockTable[blockNb].cSize,
                                ddict);
                            if (ZSTD_isError(regenSize)) {
                                DISPLAY("ZSTD_decompress_usingDDict() failed on block %u : %s  \n",
                                          blockNb, ZSTD_getErrorName(regenSize));
                                clockLoop = 0;   /* force immediate test end */
                                break;
                            }
                            blockTable[blockNb].resSize = regenSize;
                        }
                        nbLoops++;
                    } while (UTIL_clockSpanMicro(clockStart) < clockLoop);
                    ZSTD_freeDDict(ddict);
                } else if (compressor == BMK_ZSTD_STREAM) {
                    ZSTD_inBuffer inBuffer;
                    ZSTD_outBuffer outBuffer;
                    ZSTD_DStream* zbd = ZSTD_createDStream();
                    size_t rSize;
                    if (zbd == NULL) EXM_THROW(1, "ZSTD_createDStream() allocation failure");
                    rSize = ZSTD_initDStream_usingDict(zbd, dictBuffer, dictBufferSize);
                    if (ZSTD_isError(rSize)) EXM_THROW(1, "ZSTD_initDStream() failed : %s", ZSTD_getErrorName(rSize));
                    do {
                        U32 blockNb;
                        for (blockNb=0; blockNb<nbBlocks; blockNb++) {
                            rSize = ZSTD_resetDStream(zbd);
                            if (ZSTD_isError(rSize)) EXM_THROW(1, "ZSTD_resetDStream() failed : %s", ZSTD_getErrorName(rSize));
                            inBuffer.src = blockTable[blockNb].cPtr;
                            inBuffer.size = blockTable[blockNb].cSize;
                            inBuffer.pos = 0;
                            outBuffer.dst = blockTable[blockNb].resPtr;
                            outBuffer.size = blockTable[blockNb].srcSize;
                            outBuffer.pos = 0;
                            rSize = ZSTD_decompressStream(zbd, &outBuffer, &inBuffer);
                            if (ZSTD_isError(rSize)) EXM_THROW(1, "ZSTD_decompressStream() failed : %s", ZSTD_getErrorName(rSize));
                            blockTable[blockNb].resSize = outBuffer.pos;
                        }
                        nbLoops++;
                    } while (UTIL_clockSpanMicro(clockStart) < clockLoop);
                    ZSTD_freeDStream(zbd);
                } else if (compressor == BMK_ZWRAP_ZLIB_REUSE || compressor == BMK_ZWRAP_ZSTD_REUSE || compressor == BMK_ZLIB_REUSE) {
                    z_stream inf;
                    int ret;
                    if (compressor == BMK_ZLIB_REUSE) ZWRAP_setDecompressionType(ZWRAP_FORCE_ZLIB);
                    else ZWRAP_setDecompressionType(ZWRAP_AUTO);
                    inf.zalloc = Z_NULL;
                    inf.zfree = Z_NULL;
                    inf.opaque = Z_NULL;
                    ret = inflateInit(&inf);
                    if (ret != Z_OK) EXM_THROW(1, "inflateInit failure");
                    do {
                        U32 blockNb;
                        for (blockNb=0; blockNb<nbBlocks; blockNb++) {
                            if (ZWRAP_isUsingZSTDdecompression(&inf))
                                ret = ZWRAP_inflateReset_keepDict(&inf); /* reuse dictionary to make decompression faster; inflate will return Z_NEED_DICT only for the first time */
                            else
                                ret = inflateReset(&inf);
                            if (ret != Z_OK) EXM_THROW(1, "inflateReset failure");
                            inf.next_in = (z_const z_Bytef*) blockTable[blockNb].cPtr;
                            inf.avail_in = (uInt)blockTable[blockNb].cSize;
                            inf.total_in = 0;
                            inf.next_out = (z_Bytef*) blockTable[blockNb].resPtr;
                            inf.avail_out = (uInt)blockTable[blockNb].srcSize;
                            inf.total_out = 0;
                            ret = inflate(&inf, Z_FINISH);
                            if (ret == Z_NEED_DICT) {
                                ret = inflateSetDictionary(&inf, (const z_Bytef*)dictBuffer, dictBufferSize);
                                if (ret != Z_OK) EXM_THROW(1, "inflateSetDictionary failure");
                                ret = inflate(&inf, Z_FINISH);
                            }
                            if (ret != Z_STREAM_END) EXM_THROW(1, "inflate failure");
                            blockTable[blockNb].resSize = inf.total_out;
                        }
                        nbLoops++;
                    } while (UTIL_clockSpanMicro(clockStart) < clockLoop);
                    ret = inflateEnd(&inf);
                    if (ret != Z_OK) EXM_THROW(1, "inflateEnd failure");
                } else {
                    z_stream inf;
                    if (compressor == BMK_ZLIB) ZWRAP_setDecompressionType(ZWRAP_FORCE_ZLIB);
                    else ZWRAP_setDecompressionType(ZWRAP_AUTO);
                    do {
                        U32 blockNb;
                        for (blockNb=0; blockNb<nbBlocks; blockNb++) {
                            int ret;
                            inf.zalloc = Z_NULL;
                            inf.zfree = Z_NULL;
                            inf.opaque = Z_NULL;
                            ret = inflateInit(&inf);
                            if (ret != Z_OK) EXM_THROW(1, "inflateInit failure");
                            inf.next_in = (z_const z_Bytef*) blockTable[blockNb].cPtr;
                            inf.avail_in = (uInt)blockTable[blockNb].cSize;
                            inf.total_in = 0;
                            inf.next_out = (z_Bytef*) blockTable[blockNb].resPtr;
                            inf.avail_out = (uInt)blockTable[blockNb].srcSize;
                            inf.total_out = 0;
                            ret = inflate(&inf, Z_FINISH);
                            if (ret == Z_NEED_DICT) {
                                ret = inflateSetDictionary(&inf, (const z_Bytef*) dictBuffer, dictBufferSize);
                                if (ret != Z_OK) EXM_THROW(1, "inflateSetDictionary failure");
                                ret = inflate(&inf, Z_FINISH);
                            }
                            if (ret != Z_STREAM_END) EXM_THROW(1, "inflate failure");
                            ret = inflateEnd(&inf);
                            if (ret != Z_OK) EXM_THROW(1, "inflateEnd failure");
                            blockTable[blockNb].resSize = inf.total_out;
                        }
                        nbLoops++;
                    } while (UTIL_clockSpanMicro(clockStart) < clockLoop);
                }
                {   U64 const clockSpan = UTIL_clockSpanMicro(clockStart);
                    if (clockSpan < fastestD*nbLoops) fastestD = clockSpan / nbLoops;
                    totalDTime += clockSpan;
                    dCompleted = totalDTime>maxTime;
            }   }

            markNb = (markNb+1) % NB_MARKS;
            DISPLAYLEVEL(2, "%2s-%-17.17s :%10u ->%10u (%5.3f),%6.1f MB/s ,%6.1f MB/s\r",
                    marks[markNb], displayName, (unsigned)srcSize, (unsigned)cSize, ratio,
                    (double)srcSize / fastestC,
                    (double)srcSize / fastestD );

            /* CRC Checking */
            {   U64 const crcCheck = XXH64(resultBuffer, srcSize, 0);
                if (crcOrig!=crcCheck) {
                    size_t u;
                    DISPLAY("!!! WARNING !!! %14s : Invalid Checksum : %x != %x   \n", displayName, (unsigned)crcOrig, (unsigned)crcCheck);
                    for (u=0; u<srcSize; u++) {
                        if (((const BYTE*)srcBuffer)[u] != ((const BYTE*)resultBuffer)[u]) {
                            unsigned segNb, bNb, pos;
                            size_t bacc = 0;
                            DISPLAY("Decoding error at pos %u ", (unsigned)u);
                            for (segNb = 0; segNb < nbBlocks; segNb++) {
                                if (bacc + blockTable[segNb].srcSize > u) break;
                                bacc += blockTable[segNb].srcSize;
                            }
                            pos = (U32)(u - bacc);
                            bNb = pos / (128 KB);
                            DISPLAY("(block %u, sub %u, pos %u) \n", segNb, bNb, pos);
                            break;
                        }
                        if (u==srcSize-1) {  /* should never happen */
                            DISPLAY("no difference detected\n");
                    }   }
                    break;
            }   }   /* CRC Checking */
#endif
        }   /* for (testNb = 1; testNb <= (g_nbIterations + !g_nbIterations); testNb++) */

        if (g_displayLevel == 1) {
            double cSpeed = (double)srcSize / fastestC;
            double dSpeed = (double)srcSize / fastestD;
            if (g_additionalParam)
                DISPLAY("-%-3i%11i (%5.3f) %6.2f MB/s %6.1f MB/s  %s (param=%d)\n", cLevel, (int)cSize, ratio, cSpeed, dSpeed, displayName, g_additionalParam);
            else
                DISPLAY("-%-3i%11i (%5.3f) %6.2f MB/s %6.1f MB/s  %s\n", cLevel, (int)cSize, ratio, cSpeed, dSpeed, displayName);
        }
        DISPLAYLEVEL(2, "%2i#\n", cLevel);
    }   /* Bench */

    /* clean up */
    free(blockTable);
    free(compressedBuffer);
    free(resultBuffer);
    ZSTD_freeCCtx(ctx);
    ZSTD_freeDCtx(dctx);
    return 0;
}


static size_t BMK_findMaxMem(U64 requiredMem)
{
    size_t const step = 64 MB;
    BYTE* testmem = NULL;

    requiredMem = (((requiredMem >> 26) + 1) << 26);
    requiredMem += step;
    if (requiredMem > maxMemory) requiredMem = maxMemory;

    do {
        testmem = (BYTE*)malloc((size_t)requiredMem);
        requiredMem -= step;
    } while (!testmem && requiredMem);   /* do not allocate zero bytes */

    free(testmem);
    return (size_t)(requiredMem+1);  /* avoid zero */
}

static void BMK_benchCLevel(void* srcBuffer, size_t benchedSize,
                            const char* displayName, int cLevel, int cLevelLast,
                            const size_t* fileSizes, unsigned nbFiles,
                            const void* dictBuffer, size_t dictBufferSize)
{
    int l;

    const char* pch = strrchr(displayName, '\\'); /* Windows */
    if (!pch) pch = strrchr(displayName, '/'); /* Linux */
    if (pch) displayName = pch+1;

    SET_REALTIME_PRIORITY;

    if (g_displayLevel == 1 && !g_additionalParam)
        DISPLAY("bench %s %s: input %u bytes, %u seconds, %u KB blocks\n",
                ZSTD_VERSION_STRING, ZSTD_GIT_COMMIT_STRING,
                (unsigned)benchedSize, g_nbIterations, (unsigned)(g_blockSize>>10));

    if (cLevelLast < cLevel) cLevelLast = cLevel;

    DISPLAY("benchmarking zstd %s (using ZSTD_CStream)\n", ZSTD_VERSION_STRING);
    for (l=cLevel; l <= cLevelLast; l++) {
        BMK_benchMem(srcBuffer, benchedSize,
                     displayName, l,
                     fileSizes, nbFiles,
                     dictBuffer, dictBufferSize, BMK_ZSTD_STREAM);
    }

    DISPLAY("benchmarking zstd %s (using ZSTD_CCtx)\n", ZSTD_VERSION_STRING);
    for (l=cLevel; l <= cLevelLast; l++) {
        BMK_benchMem(srcBuffer, benchedSize,
                     displayName, l,
                     fileSizes, nbFiles,
                     dictBuffer, dictBufferSize, BMK_ZSTD);
    }

    DISPLAY("benchmarking zstd %s (using zlibWrapper)\n", ZSTD_VERSION_STRING);
    for (l=cLevel; l <= cLevelLast; l++) {
        BMK_benchMem(srcBuffer, benchedSize,
                     displayName, l,
                     fileSizes, nbFiles,
                     dictBuffer, dictBufferSize, BMK_ZWRAP_ZSTD_REUSE);
    }

    DISPLAY("benchmarking zstd %s (zlibWrapper not reusing a context)\n", ZSTD_VERSION_STRING);
    for (l=cLevel; l <= cLevelLast; l++) {
        BMK_benchMem(srcBuffer, benchedSize,
                     displayName, l,
                     fileSizes, nbFiles,
                     dictBuffer, dictBufferSize, BMK_ZWRAP_ZSTD);
    }


    if (cLevelLast > Z_BEST_COMPRESSION) cLevelLast = Z_BEST_COMPRESSION;

    DISPLAY("\n");
    DISPLAY("benchmarking zlib %s\n", ZLIB_VERSION);
    for (l=cLevel; l <= cLevelLast; l++) {
        BMK_benchMem(srcBuffer, benchedSize,
                     displayName, l,
                     fileSizes, nbFiles,
                     dictBuffer, dictBufferSize, BMK_ZLIB_REUSE);
    }

    DISPLAY("benchmarking zlib %s (zlib not reusing a context)\n", ZLIB_VERSION);
    for (l=cLevel; l <= cLevelLast; l++) {
        BMK_benchMem(srcBuffer, benchedSize,
                     displayName, l,
                     fileSizes, nbFiles,
                     dictBuffer, dictBufferSize, BMK_ZLIB);
    }

    DISPLAY("benchmarking zlib %s (using zlibWrapper)\n", ZLIB_VERSION);
    for (l=cLevel; l <= cLevelLast; l++) {
        BMK_benchMem(srcBuffer, benchedSize,
                     displayName, l,
                     fileSizes, nbFiles,
                     dictBuffer, dictBufferSize, BMK_ZWRAP_ZLIB_REUSE);
    }

    DISPLAY("benchmarking zlib %s (zlibWrapper not reusing a context)\n", ZLIB_VERSION);
    for (l=cLevel; l <= cLevelLast; l++) {
        BMK_benchMem(srcBuffer, benchedSize,
                     displayName, l,
                     fileSizes, nbFiles,
                     dictBuffer, dictBufferSize, BMK_ZWRAP_ZLIB);
    }
}


/*! BMK_loadFiles() :
    Loads `buffer` with content of files listed within `fileNamesTable`.
    At most, fills `buffer` entirely */
static void BMK_loadFiles(void* buffer, size_t bufferSize,
                          size_t* fileSizes,
                          const char** fileNamesTable, unsigned nbFiles)
{
    size_t pos = 0, totalSize = 0;
    unsigned n;
    for (n=0; n<nbFiles; n++) {
        FILE* f;
        U64 fileSize = UTIL_getFileSize(fileNamesTable[n]);
        if (UTIL_isDirectory(fileNamesTable[n])) {
            DISPLAYLEVEL(2, "Ignoring %s directory...       \n", fileNamesTable[n]);
            fileSizes[n] = 0;
            continue;
        }
        if (fileSize == UTIL_FILESIZE_UNKNOWN) {
            DISPLAYLEVEL(2, "Cannot determine size of %s ...    \n", fileNamesTable[n]);
            fileSizes[n] = 0;
            continue;
        }
        f = fopen(fileNamesTable[n], "rb");
        if (f==NULL) EXM_THROW(10, "impossible to open file %s", fileNamesTable[n]);
        DISPLAYUPDATE(2, "Loading %s...       \r", fileNamesTable[n]);
        if (fileSize > bufferSize-pos) fileSize = bufferSize-pos, nbFiles=n;   /* buffer too small - stop after this file */
        { size_t const readSize = fread(((char*)buffer)+pos, 1, (size_t)fileSize, f);
          if (readSize != (size_t)fileSize) EXM_THROW(11, "could not read %s", fileNamesTable[n]);
          pos += readSize; }
        fileSizes[n] = (size_t)fileSize;
        totalSize += (size_t)fileSize;
        fclose(f);
    }

    if (totalSize == 0) EXM_THROW(12, "no data to bench");
}

static void BMK_benchFileTable(const char** fileNamesTable, unsigned nbFiles,
                               const char* dictFileName, int cLevel, int cLevelLast)
{
    void* srcBuffer;
    size_t benchedSize;
    void* dictBuffer = NULL;
    size_t dictBufferSize = 0;
    size_t* fileSizes = (size_t*)malloc(nbFiles * sizeof(size_t));
    U64 const totalSizeToLoad = UTIL_getTotalFileSize(fileNamesTable, nbFiles);
    char mfName[20] = {0};

    if (!fileSizes) EXM_THROW(12, "not enough memory for fileSizes");

    /* Load dictionary */
    if (dictFileName != NULL) {
        U64 const dictFileSize = UTIL_getFileSize(dictFileName);
        if (dictFileSize > 64 MB)
            EXM_THROW(10, "dictionary file %s too large", dictFileName);
        dictBufferSize = (size_t)dictFileSize;
        dictBuffer = malloc(dictBufferSize);
        if (dictBuffer==NULL)
            EXM_THROW(11, "not enough memory for dictionary (%u bytes)", (unsigned)dictBufferSize);
        BMK_loadFiles(dictBuffer, dictBufferSize, fileSizes, &dictFileName, 1);
    }

    /* Memory allocation & restrictions */
    benchedSize = BMK_findMaxMem(totalSizeToLoad * 3) / 3;
    if ((U64)benchedSize > totalSizeToLoad) benchedSize = (size_t)totalSizeToLoad;
    if (benchedSize < totalSizeToLoad)
        DISPLAY("Not enough memory; testing %u MB only...\n", (unsigned)(benchedSize >> 20));
    srcBuffer = malloc(benchedSize + !benchedSize);
    if (!srcBuffer) EXM_THROW(12, "not enough memory");

    /* Load input buffer */
    BMK_loadFiles(srcBuffer, benchedSize, fileSizes, fileNamesTable, nbFiles);

    /* Bench */
    snprintf (mfName, sizeof(mfName), " %u files", nbFiles);
    {   const char* displayName = (nbFiles > 1) ? mfName : fileNamesTable[0];
        BMK_benchCLevel(srcBuffer, benchedSize,
                        displayName, cLevel, cLevelLast,
                        fileSizes, nbFiles,
                        dictBuffer, dictBufferSize);
    }

    /* clean up */
    free(srcBuffer);
    free(dictBuffer);
    free(fileSizes);
}


static void BMK_syntheticTest(int cLevel, int cLevelLast, double compressibility)
{
    char name[20] = {0};
    size_t benchedSize = 10000000;
    void* const srcBuffer = malloc(benchedSize);

    /* Memory allocation */
    if (!srcBuffer) EXM_THROW(21, "not enough memory");

    /* Fill input buffer */
    RDG_genBuffer(srcBuffer, benchedSize, compressibility, 0.0, 0);

    /* Bench */
    snprintf (name, sizeof(name), "Synthetic %2u%%", (unsigned)(compressibility*100));
    BMK_benchCLevel(srcBuffer, benchedSize, name, cLevel, cLevelLast, &benchedSize, 1, NULL, 0);

    /* clean up */
    free(srcBuffer);
}


int BMK_benchFiles(const char** fileNamesTable, unsigned nbFiles,
                   const char* dictFileName, int cLevel, int cLevelLast)
{
    double const compressibility = (double)g_compressibilityDefault / 100;

    if (nbFiles == 0)
        BMK_syntheticTest(cLevel, cLevelLast, compressibility);
    else
        BMK_benchFileTable(fileNamesTable, nbFiles, dictFileName, cLevel, cLevelLast);
    return 0;
}




/*-************************************
*  Command Line
**************************************/
static int usage(const char* programName)
{
    DISPLAY(WELCOME_MESSAGE);
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [args] [FILE(s)] [-o file]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "FILE    : a filename\n");
    DISPLAY( "          with no FILE, or when FILE is - , read standard input\n");
    DISPLAY( "Arguments :\n");
    DISPLAY( " -D file: use `file` as Dictionary \n");
    DISPLAY( " -h/-H  : display help/long help and exit\n");
    DISPLAY( " -V     : display Version number and exit\n");
    DISPLAY( " -v     : verbose mode; specify multiple times to increase log level (default:%d)\n", DEFAULT_DISPLAY_LEVEL);
    DISPLAY( " -q     : suppress warnings; specify twice to suppress errors too\n");
#ifdef UTIL_HAS_CREATEFILELIST
    DISPLAY( " -r     : operate recursively on directories\n");
#endif
    DISPLAY( "\n");
    DISPLAY( "Benchmark arguments :\n");
    DISPLAY( " -b#    : benchmark file(s), using # compression level (default : %d) \n", ZSTDCLI_CLEVEL_DEFAULT);
    DISPLAY( " -e#    : test all compression levels from -bX to # (default: %d)\n", ZSTDCLI_CLEVEL_DEFAULT);
    DISPLAY( " -i#    : minimum evaluation time in seconds (default : 3s)\n");
    DISPLAY( " -B#    : cut file into independent blocks of size # (default: no block)\n");
    return 0;
}

static int badusage(const char* programName)
{
    DISPLAYLEVEL(1, "Incorrect parameters\n");
    if (g_displayLevel >= 1) usage(programName);
    return 1;
}

static void waitEnter(void)
{
    int unused;
    DISPLAY("Press enter to continue...\n");
    unused = getchar();
    (void)unused;
}

/*! readU32FromChar() :
    @return : unsigned integer value reach from input in `char` format
    Will also modify `*stringPtr`, advancing it to position where it stopped reading.
    Note : this function can overflow if digit string > MAX_UINT */
static unsigned readU32FromChar(const char** stringPtr)
{
    unsigned result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9'))
        result *= 10, result += **stringPtr - '0', (*stringPtr)++ ;
    return result;
}


#define CLEAN_RETURN(i) { operationResult = (i); goto _end; }

int main(int argCount, char** argv)
{
    int argNb,
        main_pause=0,
        nextEntryIsDictionary=0,
        operationResult=0,
        nextArgumentIsFile=0;
    int cLevel = ZSTDCLI_CLEVEL_DEFAULT;
    int cLevelLast = 1;
    unsigned recursive = 0;
    const char** filenameTable = (const char**)malloc(argCount * sizeof(const char*));   /* argCount >= 1 */
    unsigned filenameIdx = 0;
    const char* programName = argv[0];
    const char* dictFileName = NULL;
    char* dynNameSpace = NULL;
#ifdef UTIL_HAS_CREATEFILELIST
    const char** fileNamesTable = NULL;
    char* fileNamesBuf = NULL;
    unsigned fileNamesNb;
#endif

    /* init */
    if (filenameTable==NULL) { DISPLAY("zstd: %s \n", strerror(errno)); exit(1); }
    displayOut = stderr;

    /* Pick out program name from path. Don't rely on stdlib because of conflicting behavior */
    {   size_t pos;
        for (pos = (int)strlen(programName); pos > 0; pos--) { if (programName[pos] == '/') { pos++; break; } }
        programName += pos;
    }

     /* command switches */
    for(argNb=1; argNb<argCount; argNb++) {
        const char* argument = argv[argNb];
        if(!argument) continue;   /* Protection if argument empty */

        if (nextArgumentIsFile==0) {

            /* long commands (--long-word) */
            if (!strcmp(argument, "--")) { nextArgumentIsFile=1; continue; }
            if (!strcmp(argument, "--version")) { displayOut=stdout; DISPLAY(WELCOME_MESSAGE); CLEAN_RETURN(0); }
            if (!strcmp(argument, "--help")) { displayOut=stdout; CLEAN_RETURN(usage(programName)); }
            if (!strcmp(argument, "--verbose")) { g_displayLevel++; continue; }
            if (!strcmp(argument, "--quiet")) { g_displayLevel--; continue; }

            /* Decode commands (note : aggregated commands are allowed) */
            if (argument[0]=='-') {
                argument++;

                while (argument[0]!=0) {
                    switch(argument[0])
                    {
                        /* Display help */
                    case 'V': displayOut=stdout; DISPLAY(WELCOME_MESSAGE); CLEAN_RETURN(0);   /* Version Only */
                    case 'H':
                    case 'h': displayOut=stdout; CLEAN_RETURN(usage(programName));

                        /* Use file content as dictionary */
                    case 'D': nextEntryIsDictionary = 1; argument++; break;

                        /* Verbose mode */
                    case 'v': g_displayLevel++; argument++; break;

                        /* Quiet mode */
                    case 'q': g_displayLevel--; argument++; break;

#ifdef UTIL_HAS_CREATEFILELIST
                        /* recursive */
                    case 'r': recursive=1; argument++; break;
#endif

                        /* Benchmark */
                    case 'b':
                            /* first compression Level */
                            argument++;
                            cLevel = readU32FromChar(&argument);
                            break;

                        /* range bench (benchmark only) */
                    case 'e':
                            /* last compression Level */
                            argument++;
                            cLevelLast = readU32FromChar(&argument);
                            break;

                        /* Modify Nb Iterations (benchmark only) */
                    case 'i':
                        argument++;
                        {   U32 const iters = readU32FromChar(&argument);
                            BMK_setNotificationLevel(g_displayLevel);
                            BMK_SetNbIterations(iters);
                        }
                        break;

                        /* cut input into blocks (benchmark only) */
                    case 'B':
                        argument++;
                        {   size_t bSize = readU32FromChar(&argument);
                            if (toupper(*argument)=='K') bSize<<=10, argument++;  /* allows using KB notation */
                            if (toupper(*argument)=='M') bSize<<=20, argument++;
                            if (toupper(*argument)=='B') argument++;
                            BMK_setNotificationLevel(g_displayLevel);
                            BMK_SetBlockSize(bSize);
                        }
                        break;

                        /* Pause at the end (-p) or set an additional param (-p#) (hidden option) */
                    case 'p': argument++;
                        if ((*argument>='0') && (*argument<='9')) {
                            BMK_setAdditionalParam(readU32FromChar(&argument));
                        } else
                            main_pause=1;
                        break;
                        /* unknown command */
                    default : CLEAN_RETURN(badusage(programName));
                    }
                }
                continue;
            }   /* if (argument[0]=='-') */

        }   /* if (nextArgumentIsAFile==0) */

        if (nextEntryIsDictionary) {
            nextEntryIsDictionary = 0;
            dictFileName = argument;
            continue;
        }

        /* add filename to list */
        filenameTable[filenameIdx++] = argument;
    }

    /* Welcome message (if verbose) */
    DISPLAYLEVEL(3, WELCOME_MESSAGE);

#ifdef UTIL_HAS_CREATEFILELIST
    if (recursive) {
        fileNamesTable = UTIL_createFileList(filenameTable, filenameIdx, &fileNamesBuf, &fileNamesNb, 1);
        if (fileNamesTable) {
            unsigned u;
            for (u=0; u<fileNamesNb; u++) DISPLAYLEVEL(4, "%u %s\n", u, fileNamesTable[u]);
            free((void*)filenameTable);
            filenameTable = fileNamesTable;
            filenameIdx = fileNamesNb;
        }
    }
#endif

    BMK_setNotificationLevel(g_displayLevel);
    BMK_benchFiles(filenameTable, filenameIdx, dictFileName, cLevel, cLevelLast);

_end:
    if (main_pause) waitEnter();
    free(dynNameSpace);
#ifdef UTIL_HAS_CREATEFILELIST
    if (fileNamesTable)
        UTIL_freeFileList(fileNamesTable, fileNamesBuf);
    else
#endif
        free((void*)filenameTable);
    return operationResult;
}
