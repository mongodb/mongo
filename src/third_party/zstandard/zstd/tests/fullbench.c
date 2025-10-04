/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


/*_************************************
*  Includes
**************************************/
#define ZSTD_DISABLE_DEPRECATE_WARNINGS /* No deprecation warnings, we still bench some deprecated functions */
#include "util.h"        /* Compiler options, UTIL_GetFileSize */
#include <stdlib.h>      /* malloc */
#include <stdio.h>       /* fprintf, fopen, ftello64 */
#include <assert.h>

#include "timefn.h"      /* UTIL_clockSpanNano, UTIL_getTime */
#include "mem.h"         /* U32 */
#ifndef ZSTD_DLL_IMPORT
    #include "zstd_internal.h"   /* ZSTD_decodeSeqHeaders, ZSTD_blockHeaderSize, ZSTD_getcBlockSize, blockType_e, KB, MB */
    #include "decompress/zstd_decompress_internal.h"   /* ZSTD_DCtx struct */
#else
    #define KB *(1 <<10)
    #define MB *(1 <<20)
    #define GB *(1U<<30)
    typedef enum { bt_raw, bt_rle, bt_compressed, bt_reserved } blockType_e;
#endif
#define ZSTD_STATIC_LINKING_ONLY  /* ZSTD_compressBegin, ZSTD_compressContinue, etc. */
#include "zstd.h"        /* ZSTD_versionString */
#include "util.h"        /* time functions */
#include "datagen.h"
#include "benchfn.h"     /* CustomBench */
#include "benchzstd.h"   /* MB_UNIT */


/*_************************************
*  Constants
**************************************/
#define PROGRAM_DESCRIPTION "Zstandard speed analyzer"
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s %s %i-bits, by %s (%s) ***\n", PROGRAM_DESCRIPTION, ZSTD_versionString(), (int)(sizeof(void*)*8), AUTHOR, __DATE__

#define NBLOOPS    6
#define TIMELOOP_S 2

#define MAX_MEM    (1984 MB)

#define DEFAULT_CLEVEL 1

#define COMPRESSIBILITY_DEFAULT 0.50
static const size_t kSampleSizeDefault = 10000000;

#define TIMELOOP_NANOSEC      (1*1000000000ULL) /* 1 second */


/*_************************************
*  Macros
**************************************/
#define DISPLAY(...)  fprintf(stderr, __VA_ARGS__)

#define CONTROL(c)  { if (!(c)) { abort(); } }   /* like assert(), but cannot be disabled */

/*_************************************
*  Benchmark Parameters
**************************************/
static unsigned g_nbIterations = NBLOOPS;


/*_*******************************************************
*  Private functions
*********************************************************/
static size_t BMK_findMaxMem(U64 requiredMem)
{
    size_t const step = 64 MB;
    void* testmem = NULL;

    requiredMem = (((requiredMem >> 26) + 1) << 26);
    if (requiredMem > MAX_MEM) requiredMem = MAX_MEM;

    requiredMem += step;
    do {
        testmem = malloc ((size_t)requiredMem);
        requiredMem -= step;
    } while (!testmem);

    free (testmem);
    return (size_t) requiredMem;
}


/*_*******************************************************
*  Benchmark wrappers
*********************************************************/

static ZSTD_CCtx* g_zcc = NULL;

static size_t
local_ZSTD_compress(const void* src, size_t srcSize,
                    void* dst, size_t dstSize,
                    void* payload)
{
    ZSTD_parameters p;
    ZSTD_frameParameters f = { 1 /* contentSizeHeader*/, 0, 0 };
    p.fParams = f;
    p.cParams = *(ZSTD_compressionParameters*)payload;
    return ZSTD_compress_advanced (g_zcc, dst, dstSize, src, srcSize, NULL ,0, p);
}

static size_t
local_ZSTD_compress_freshCCtx(const void* src, size_t srcSize,
                    void* dst, size_t dstSize,
                    void* payload)
{
    ZSTD_parameters p;
    ZSTD_frameParameters f = { 1 /* contentSizeHeader*/, 0, 0 };
    p.fParams = f;
    p.cParams = *(ZSTD_compressionParameters*)payload;
    if (g_zcc != NULL) ZSTD_freeCCtx(g_zcc);
    g_zcc = ZSTD_createCCtx();
    assert(g_zcc != NULL);
    {   size_t const r = ZSTD_compress_advanced (g_zcc, dst, dstSize, src, srcSize, NULL ,0, p);
        ZSTD_freeCCtx(g_zcc);
        g_zcc = NULL;
        return r;
    }
}

static size_t g_cSize = 0;
static size_t local_ZSTD_decompress(const void* src, size_t srcSize,
                                    void* dst, size_t dstSize,
                                    void* buff2)
{
    (void)src; (void)srcSize;
    return ZSTD_decompress(dst, dstSize, buff2, g_cSize);
}

static ZSTD_DCtx* g_zdc = NULL;

#ifndef ZSTD_DLL_IMPORT
typedef enum {
    not_streaming = 0,
    is_streaming = 1
} streaming_operation;
extern size_t ZSTD_decodeLiteralsBlock(ZSTD_DCtx* ctx, const void* src, size_t srcSize, void* dst, size_t dstCapacity, const streaming_operation streaming);
static size_t local_ZSTD_decodeLiteralsBlock(const void* src, size_t srcSize, void* dst, size_t dstSize, void* buff2)
{
    (void)src; (void)srcSize; (void)dst; (void)dstSize;
    return ZSTD_decodeLiteralsBlock(g_zdc, buff2, g_cSize, dst, dstSize, not_streaming);
}

static size_t local_ZSTD_decodeSeqHeaders(const void* src, size_t srcSize, void* dst, size_t dstSize, void* buff2)
{
    int nbSeq;
    (void)src; (void)srcSize; (void)dst; (void)dstSize;
    return ZSTD_decodeSeqHeaders(g_zdc, &nbSeq, buff2, g_cSize);
}

FORCE_NOINLINE size_t ZSTD_decodeLiteralsHeader(ZSTD_DCtx* dctx, void const* src, size_t srcSize)
{
    RETURN_ERROR_IF(srcSize < MIN_CBLOCK_SIZE, corruption_detected, "");
    {
        BYTE const* istart = (BYTE const*)src;
        symbolEncodingType_e const litEncType = (symbolEncodingType_e)(istart[0] & 3);
        if (litEncType == set_compressed) {
            RETURN_ERROR_IF(srcSize < 5, corruption_detected, "srcSize >= MIN_CBLOCK_SIZE == 3; here we need up to 5 for case 3");
            {
                size_t lhSize, litSize, litCSize;
                U32 const lhlCode = (istart[0] >> 2) & 3;
                U32 const lhc = MEM_readLE32(istart);
                int const flags = ZSTD_DCtx_get_bmi2(dctx) ? HUF_flags_bmi2 : 0;
                switch(lhlCode)
                {
                case 0: case 1: default:   /* note : default is impossible, since lhlCode into [0..3] */
                    /* 2 - 2 - 10 - 10 */
                    lhSize = 3;
                    litSize  = (lhc >> 4) & 0x3FF;
                    litCSize = (lhc >> 14) & 0x3FF;
                    break;
                case 2:
                    /* 2 - 2 - 14 - 14 */
                    lhSize = 4;
                    litSize  = (lhc >> 4) & 0x3FFF;
                    litCSize = lhc >> 18;
                    break;
                case 3:
                    /* 2 - 2 - 18 - 18 */
                    lhSize = 5;
                    litSize  = (lhc >> 4) & 0x3FFFF;
                    litCSize = (lhc >> 22) + ((size_t)istart[4] << 10);
                    break;
                }
                RETURN_ERROR_IF(litSize > ZSTD_BLOCKSIZE_MAX, corruption_detected, "");
                RETURN_ERROR_IF(litCSize + lhSize > srcSize, corruption_detected, "");
#ifndef HUF_FORCE_DECOMPRESS_X2
                return HUF_readDTableX1_wksp(
                        dctx->entropy.hufTable,
                        istart+lhSize, litCSize,
                        dctx->workspace, sizeof(dctx->workspace),
                        flags);
#else
                return HUF_readDTableX2_wksp(
                        dctx->entropy.hufTable,
                        istart+lhSize, litCSize,
                        dctx->workspace, sizeof(dctx->workspace), flags);
#endif
            }
        }
    }
    return 0;
}

static size_t local_ZSTD_decodeLiteralsHeader(const void* src, size_t srcSize, void* dst, size_t dstSize, void* buff2)
{
    (void)dst, (void)dstSize, (void)src, (void)srcSize;
    return ZSTD_decodeLiteralsHeader(g_zdc, buff2, g_cSize);
}
#endif

static ZSTD_CStream* g_cstream= NULL;
static size_t
local_ZSTD_compressStream(const void* src, size_t srcSize,
                          void* dst, size_t dstCapacity,
                          void* payload)
{
    ZSTD_outBuffer buffOut;
    ZSTD_inBuffer buffIn;
    ZSTD_parameters p;
    ZSTD_frameParameters f = {1 /* contentSizeHeader*/, 0, 0};
    p.fParams = f;
    p.cParams = *(ZSTD_compressionParameters*)payload;
    ZSTD_initCStream_advanced(g_cstream, NULL, 0, p, ZSTD_CONTENTSIZE_UNKNOWN);
    buffOut.dst = dst;
    buffOut.size = dstCapacity;
    buffOut.pos = 0;
    buffIn.src = src;
    buffIn.size = srcSize;
    buffIn.pos = 0;
    ZSTD_compressStream(g_cstream, &buffOut, &buffIn);
    ZSTD_endStream(g_cstream, &buffOut);
    return buffOut.pos;
}

static size_t
local_ZSTD_compressStream_freshCCtx(const void* src, size_t srcSize,
                          void* dst, size_t dstCapacity,
                          void* payload)
{
    if (g_cstream != NULL) ZSTD_freeCCtx(g_cstream);
    g_cstream = ZSTD_createCCtx();
    assert(g_cstream != NULL);

    {   size_t const r = local_ZSTD_compressStream(src, srcSize, dst, dstCapacity, payload);
        ZSTD_freeCCtx(g_cstream);
        g_cstream = NULL;
        return r;
    }
}

static size_t
local_ZSTD_compress2(const void* src, size_t srcSize,
                           void* dst, size_t dstCapacity,
                           void* payload)
{
    (void)payload;
    return ZSTD_compress2(g_cstream, dst, dstCapacity, src, srcSize);
}

static size_t
local_ZSTD_compressStream2_end(const void* src, size_t srcSize,
    void* dst, size_t dstCapacity,
    void* payload)
{
    ZSTD_outBuffer buffOut;
    ZSTD_inBuffer buffIn;
    (void)payload;
    buffOut.dst = dst;
    buffOut.size = dstCapacity;
    buffOut.pos = 0;
    buffIn.src = src;
    buffIn.size = srcSize;
    buffIn.pos = 0;
    ZSTD_compressStream2(g_cstream, &buffOut, &buffIn, ZSTD_e_end);
    return buffOut.pos;
}

static size_t
local_ZSTD_compressStream2_continue(const void* src, size_t srcSize,
                                 void* dst, size_t dstCapacity,
                                 void* payload)
{
    ZSTD_outBuffer buffOut;
    ZSTD_inBuffer buffIn;
    (void)payload;
    buffOut.dst = dst;
    buffOut.size = dstCapacity;
    buffOut.pos = 0;
    buffIn.src = src;
    buffIn.size = srcSize;
    buffIn.pos = 0;
    ZSTD_compressStream2(g_cstream, &buffOut, &buffIn, ZSTD_e_continue);
    ZSTD_compressStream2(g_cstream, &buffOut, &buffIn, ZSTD_e_end);
    return buffOut.pos;
}

static size_t
local_ZSTD_compress_generic_T2_end(const void* src, size_t srcSize,
                                   void* dst, size_t dstCapacity,
                                   void* payload)
{
    (void)payload;
    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_nbWorkers, 2);
    return ZSTD_compress2(g_cstream, dst, dstCapacity, src, srcSize);
}

static size_t
local_ZSTD_compress_generic_T2_continue(const void* src, size_t srcSize,
                                        void* dst, size_t dstCapacity,
                                        void* payload)
{
    ZSTD_outBuffer buffOut;
    ZSTD_inBuffer buffIn;
    (void)payload;
    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_nbWorkers, 2);
    buffOut.dst = dst;
    buffOut.size = dstCapacity;
    buffOut.pos = 0;
    buffIn.src = src;
    buffIn.size = srcSize;
    buffIn.pos = 0;
    ZSTD_compressStream2(g_cstream, &buffOut, &buffIn, ZSTD_e_continue);
    while(ZSTD_compressStream2(g_cstream, &buffOut, &buffIn, ZSTD_e_end)) {}
    return buffOut.pos;
}

static ZSTD_DStream* g_dstream= NULL;
static size_t
local_ZSTD_decompressStream(const void* src, size_t srcSize,
                            void* dst, size_t dstCapacity,
                            void* buff2)
{
    ZSTD_outBuffer buffOut;
    ZSTD_inBuffer buffIn;
    (void)src; (void)srcSize;
    ZSTD_initDStream(g_dstream);
    buffOut.dst = dst;
    buffOut.size = dstCapacity;
    buffOut.pos = 0;
    buffIn.src = buff2;
    buffIn.size = g_cSize;
    buffIn.pos = 0;
    ZSTD_decompressStream(g_dstream, &buffOut, &buffIn);
    return buffOut.pos;
}

#ifndef ZSTD_DLL_IMPORT
static size_t local_ZSTD_compressContinue(const void* src, size_t srcSize,
                                          void* dst, size_t dstCapacity,
                                          void* payload)
{
    ZSTD_parameters p;
    ZSTD_frameParameters f = { 1 /* contentSizeHeader*/, 0, 0 };
    p.fParams = f;
    p.cParams = *(ZSTD_compressionParameters*)payload;
    ZSTD_compressBegin_advanced(g_zcc, NULL, 0, p, srcSize);
    return ZSTD_compressEnd(g_zcc, dst, dstCapacity, src, srcSize);
}

#define FIRST_BLOCK_SIZE 8
static size_t
local_ZSTD_compressContinue_extDict(const void* src, size_t srcSize,
                                    void* dst, size_t dstCapacity,
                                    void* payload)
{
    BYTE firstBlockBuf[FIRST_BLOCK_SIZE];

    ZSTD_parameters p;
    ZSTD_frameParameters const f = { 1, 0, 0 };
    p.fParams = f;
    p.cParams = *(ZSTD_compressionParameters*)payload;
    ZSTD_compressBegin_advanced(g_zcc, NULL, 0, p, srcSize);
    memcpy(firstBlockBuf, src, FIRST_BLOCK_SIZE);

    {   size_t const compressResult = ZSTD_compressContinue(g_zcc,
                                            dst, dstCapacity,
                                            firstBlockBuf, FIRST_BLOCK_SIZE);
        if (ZSTD_isError(compressResult)) {
            DISPLAY("local_ZSTD_compressContinue_extDict error : %s\n",
                    ZSTD_getErrorName(compressResult));
            return compressResult;
        }
        dst = (BYTE*)dst + compressResult;
        dstCapacity -= compressResult;
    }
    return ZSTD_compressEnd(g_zcc, dst, dstCapacity,
                            (const BYTE*)src + FIRST_BLOCK_SIZE,
                            srcSize - FIRST_BLOCK_SIZE);
}

static size_t local_ZSTD_decompressContinue(const void* src, size_t srcSize,
                                            void* dst, size_t dstCapacity,
                                            void* buff2)
{
    size_t regeneratedSize = 0;
    const BYTE* ip = (const BYTE*)buff2;
    const BYTE* const iend = ip + g_cSize;
    BYTE* op = (BYTE*)dst;
    size_t remainingCapacity = dstCapacity;

    (void)src; (void)srcSize;  /* unused */
    ZSTD_decompressBegin(g_zdc);
    while (ip < iend) {
        size_t const iSize = ZSTD_nextSrcSizeToDecompress(g_zdc);
        size_t const decodedSize = ZSTD_decompressContinue(g_zdc, op, remainingCapacity, ip, iSize);
        ip += iSize;
        regeneratedSize += decodedSize;
        op += decodedSize;
        remainingCapacity -= decodedSize;
    }

    return regeneratedSize;
}
#endif


/*_*******************************************************
*  Bench functions
*********************************************************/
static int benchMem(unsigned benchNb,
                    const void* src, size_t srcSize,
                    int cLevel, ZSTD_compressionParameters cparams)
{
    size_t dstBuffSize = ZSTD_compressBound(srcSize);
    BYTE*  dstBuff;
    void*  dstBuff2;
    void*  payload;
    const char* benchName;
    BMK_benchFn_t benchFunction;
    int errorcode = 0;

    /* Selection */
    switch(benchNb)
    {
    case 1:
        benchFunction = local_ZSTD_compress; benchName = "compress";
        break;
    case 2:
        benchFunction = local_ZSTD_decompress; benchName = "decompress";
        break;
    case 3:
        benchFunction = local_ZSTD_compress_freshCCtx; benchName = "compress_freshCCtx";
        break;
#ifndef ZSTD_DLL_IMPORT
    case 11:
        benchFunction = local_ZSTD_compressContinue; benchName = "compressContinue";
        break;
    case 12:
        benchFunction = local_ZSTD_compressContinue_extDict; benchName = "compressContinue_extDict";
        break;
    case 13:
        benchFunction = local_ZSTD_decompressContinue; benchName = "decompressContinue";
        break;
    case 30:
        benchFunction = local_ZSTD_decodeLiteralsHeader; benchName = "decodeLiteralsHeader";
        break;
    case 31:
        benchFunction = local_ZSTD_decodeLiteralsBlock; benchName = "decodeLiteralsBlock";
        break;
    case 32:
        benchFunction = local_ZSTD_decodeSeqHeaders; benchName = "decodeSeqHeaders";
        break;
#endif
    case 41:
        benchFunction = local_ZSTD_compressStream; benchName = "compressStream";
        break;
    case 42:
        benchFunction = local_ZSTD_decompressStream; benchName = "decompressStream";
        break;
    case 43:
        benchFunction = local_ZSTD_compressStream_freshCCtx; benchName = "compressStream_freshCCtx";
        break;
    case 50:
        benchFunction = local_ZSTD_compress2; benchName = "compress2";
        break;
    case 51:
        benchFunction = local_ZSTD_compressStream2_end; benchName = "compressStream2, end";
        break;
    case 52:
        benchFunction = local_ZSTD_compressStream2_end; benchName = "compressStream2, end & short";
        break;
    case 53:
        benchFunction = local_ZSTD_compressStream2_continue; benchName = "compressStream2, continue";
        break;
    case 61:
        benchFunction = local_ZSTD_compress_generic_T2_continue; benchName = "compress_generic, -T2, continue";
        break;
    case 62:
        benchFunction = local_ZSTD_compress_generic_T2_end; benchName = "compress_generic, -T2, end";
        break;
    default :
        return 0;
    }

    /* Allocation */
    dstBuff = (BYTE*)malloc(dstBuffSize);
    dstBuff2 = malloc(dstBuffSize);
    if ((!dstBuff) || (!dstBuff2)) {
        DISPLAY("\nError: not enough memory!\n");
        free(dstBuff); free(dstBuff2);
        return 12;
    }
    payload = dstBuff2;
    if (g_zcc==NULL) g_zcc = ZSTD_createCCtx();
    if (g_zdc==NULL) g_zdc = ZSTD_createDCtx();
    if (g_cstream==NULL) g_cstream = ZSTD_createCStream();
    if (g_dstream==NULL) g_dstream = ZSTD_createDStream();

    /* DISPLAY("params: cLevel %d, wlog %d hlog %d clog %d slog %d mml %d tlen %d strat %d \n",
          cLevel, cparams->windowLog, cparams->hashLog, cparams->chainLog, cparams->searchLog,
          cparams->minMatch, cparams->targetLength, cparams->strategy); */

    ZSTD_CCtx_setParameter(g_zcc, ZSTD_c_compressionLevel, cLevel);
    ZSTD_CCtx_setParameter(g_zcc, ZSTD_c_windowLog, (int)cparams.windowLog);
    ZSTD_CCtx_setParameter(g_zcc, ZSTD_c_hashLog, (int)cparams.hashLog);
    ZSTD_CCtx_setParameter(g_zcc, ZSTD_c_chainLog, (int)cparams.chainLog);
    ZSTD_CCtx_setParameter(g_zcc, ZSTD_c_searchLog, (int)cparams.searchLog);
    ZSTD_CCtx_setParameter(g_zcc, ZSTD_c_minMatch, (int)cparams.minMatch);
    ZSTD_CCtx_setParameter(g_zcc, ZSTD_c_targetLength, (int)cparams.targetLength);
    ZSTD_CCtx_setParameter(g_zcc, ZSTD_c_strategy, cparams.strategy);

    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_compressionLevel, cLevel);
    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_windowLog, (int)cparams.windowLog);
    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_hashLog, (int)cparams.hashLog);
    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_chainLog, (int)cparams.chainLog);
    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_searchLog, (int)cparams.searchLog);
    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_minMatch, (int)cparams.minMatch);
    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_targetLength, (int)cparams.targetLength);
    ZSTD_CCtx_setParameter(g_cstream, ZSTD_c_strategy, cparams.strategy);

    /* Preparation */
    switch(benchNb)
    {
    case 1:
        payload = &cparams;
        break;
    case 2:
        g_cSize = ZSTD_compress(dstBuff2, dstBuffSize, src, srcSize, cLevel);
        break;
    case 3:
        payload = &cparams;
        break;
#ifndef ZSTD_DLL_IMPORT
    case 11:
        payload = &cparams;
        break;
    case 12:
        payload = &cparams;
        break;
    case 13 :
        g_cSize = ZSTD_compress(dstBuff2, dstBuffSize, src, srcSize, cLevel);
        break;
    case 30:  /* ZSTD_decodeLiteralsHeader */
        /* fall-through */
    case 31:  /* ZSTD_decodeLiteralsBlock : starts literals block in dstBuff2 */
        {   size_t frameHeaderSize;
            g_cSize = ZSTD_compress(dstBuff, dstBuffSize, src, srcSize, cLevel);
            frameHeaderSize = ZSTD_frameHeaderSize(dstBuff, ZSTD_FRAMEHEADERSIZE_PREFIX(ZSTD_f_zstd1));
            CONTROL(!ZSTD_isError(frameHeaderSize));
            /* check block is compressible, hence contains a literals section */
            {   blockProperties_t bp;
                ZSTD_getcBlockSize(dstBuff+frameHeaderSize, dstBuffSize, &bp);  /* Get 1st block type */
                if (bp.blockType != bt_compressed) {
                    DISPLAY("ZSTD_decodeLiteralsBlock : impossible to test on this sample (not compressible)\n");
                    goto _cleanOut;
            }   }
            {   size_t const skippedSize = frameHeaderSize + ZSTD_blockHeaderSize;
                memcpy(dstBuff2, dstBuff+skippedSize, g_cSize-skippedSize);
            }
            srcSize = srcSize > 128 KB ? 128 KB : srcSize;    /* speed relative to block */
            ZSTD_decompressBegin(g_zdc);
            break;
        }
    case 32:   /* ZSTD_decodeSeqHeaders */
        {   blockProperties_t bp;
            const BYTE* ip = dstBuff;
            const BYTE* iend;
            {   size_t const cSize = ZSTD_compress(dstBuff, dstBuffSize, src, srcSize, cLevel);
                CONTROL(cSize > ZSTD_FRAMEHEADERSIZE_PREFIX(ZSTD_f_zstd1));
            }
            /* Skip frame Header */
            {   size_t const frameHeaderSize = ZSTD_frameHeaderSize(dstBuff, ZSTD_FRAMEHEADERSIZE_PREFIX(ZSTD_f_zstd1));
                CONTROL(!ZSTD_isError(frameHeaderSize));
                ip += frameHeaderSize;
            }
            /* Find end of block */
            {   size_t const cBlockSize = ZSTD_getcBlockSize(ip, dstBuffSize, &bp);   /* Get 1st block type */
                if (bp.blockType != bt_compressed) {
                    DISPLAY("ZSTD_decodeSeqHeaders : impossible to test on this sample (not compressible)\n");
                    goto _cleanOut;
                }
                iend = ip + ZSTD_blockHeaderSize + cBlockSize;   /* End of first block */
            }
            ip += ZSTD_blockHeaderSize;    /* skip block header */
            ZSTD_decompressBegin(g_zdc);
            CONTROL(iend > ip);
            ip += ZSTD_decodeLiteralsBlock(g_zdc, ip, (size_t)(iend-ip), dstBuff, dstBuffSize, not_streaming);   /* skip literal segment */
            g_cSize = (size_t)(iend-ip);
            memcpy(dstBuff2, ip, g_cSize);   /* copy rest of block (it starts by SeqHeader) */
            srcSize = srcSize > 128 KB ? 128 KB : srcSize;   /* speed relative to block */
            break;
        }
#else
    case 31:
        goto _cleanOut;
#endif
    case 41 :
        payload = &cparams;
        break;
    case 42 :
        g_cSize = ZSTD_compress(payload, dstBuffSize, src, srcSize, cLevel);
        break;
    case 43 :
        payload = &cparams;
        break;

    case 52 :
        /* compressStream2, short dstCapacity */
        dstBuffSize--;
        break;

    /* test functions */
    /* convention: test functions have ID > 100 */

    default : ;
    }

     /* warming up dstBuff */
    { size_t i; for (i=0; i<dstBuffSize; i++) dstBuff[i]=(BYTE)i; }

    /* benchmark loop */
    {   BMK_timedFnState_t* const tfs = BMK_createTimedFnState(g_nbIterations * 1000, 1000);
        void* const avoidStrictAliasingPtr = &dstBuff;
        BMK_benchParams_t bp;
        BMK_runTime_t bestResult;
        bestResult.sumOfReturn = 0;
        bestResult.nanoSecPerRun = (double)TIMELOOP_NANOSEC * 2000000000;  /* hopefully large enough : must be larger than any potential measurement */
        CONTROL(tfs != NULL);

        bp.benchFn = benchFunction;
        bp.benchPayload = payload;
        bp.initFn = NULL;
        bp.initPayload = NULL;
        bp.errorFn = ZSTD_isError;
        bp.blockCount = 1;
        bp.srcBuffers = &src;
        bp.srcSizes = &srcSize;
        bp.dstBuffers = (void* const*) avoidStrictAliasingPtr;  /* circumvent strict aliasing warning on gcc-8,
                                                                 * because gcc considers that `void* const *`  and `void**` are 2 different types */
        bp.dstCapacities = &dstBuffSize;
        bp.blockResults = NULL;

        for (;;) {
            BMK_runOutcome_t const bOutcome = BMK_benchTimedFn(tfs, bp);

            if (!BMK_isSuccessful_runOutcome(bOutcome)) {
                DISPLAY("ERROR benchmarking function ! ! \n");
                errorcode = 1;
                goto _cleanOut;
            }

            {   BMK_runTime_t const newResult = BMK_extract_runTime(bOutcome);
                if (newResult.nanoSecPerRun < bestResult.nanoSecPerRun )
                    bestResult.nanoSecPerRun = newResult.nanoSecPerRun;
                DISPLAY("\r%2u#%-29.29s:%8.1f MB/s  (%8u) ",
                        benchNb, benchName,
                        (double)srcSize * TIMELOOP_NANOSEC / bestResult.nanoSecPerRun / MB_UNIT,
                        (unsigned)newResult.sumOfReturn );
            }

            if ( BMK_isCompleted_TimedFn(tfs) ) break;
        }
        BMK_freeTimedFnState(tfs);
    }
    DISPLAY("\n");

_cleanOut:
    free(dstBuff);
    free(dstBuff2);
    ZSTD_freeCCtx(g_zcc); g_zcc=NULL;
    ZSTD_freeDCtx(g_zdc); g_zdc=NULL;
    ZSTD_freeCStream(g_cstream); g_cstream=NULL;
    ZSTD_freeDStream(g_dstream); g_dstream=NULL;
    return errorcode;
}


static int benchSample(U32 benchNb,
                       size_t benchedSize, double compressibility,
                       int cLevel, ZSTD_compressionParameters cparams)
{
    /* Allocation */
    void* const origBuff = malloc(benchedSize);
    if (!origBuff) { DISPLAY("\nError: not enough memory!\n"); return 12; }

    /* Fill buffer */
    RDG_genBuffer(origBuff, benchedSize, compressibility, 0.0, 0);

    /* bench */
    DISPLAY("\r%70s\r", "");
    DISPLAY(" Sample %u bytes : \n", (unsigned)benchedSize);
    if (benchNb) {
        benchMem(benchNb, origBuff, benchedSize, cLevel, cparams);
    } else {  /* 0 == run all tests */
        for (benchNb=0; benchNb<100; benchNb++) {
            benchMem(benchNb, origBuff, benchedSize, cLevel, cparams);
    }   }

    free(origBuff);
    return 0;
}


static int benchFiles(U32 benchNb,
                      const char** fileNamesTable, const int nbFiles,
                      int cLevel, ZSTD_compressionParameters cparams)
{
    /* Loop for each file */
    int fileIdx;
    for (fileIdx=0; fileIdx<nbFiles; fileIdx++) {
        const char* const inFileName = fileNamesTable[fileIdx];
        FILE* const inFile = fopen( inFileName, "rb" );
        size_t benchedSize;

        /* Check file existence */
        if (inFile==NULL) { DISPLAY( "Pb opening %s\n", inFileName); return 11; }

        /* Memory allocation & restrictions */
        {   U64 const inFileSize = UTIL_getFileSize(inFileName);
            if (inFileSize == UTIL_FILESIZE_UNKNOWN) {
                DISPLAY( "Cannot measure size of %s\n", inFileName);
                fclose(inFile);
                return 11;
            }
            benchedSize = BMK_findMaxMem(inFileSize*3) / 3;
            if ((U64)benchedSize > inFileSize)
                benchedSize = (size_t)inFileSize;
            if ((U64)benchedSize < inFileSize) {
                DISPLAY("Not enough memory for '%s' full size; testing %u MB only... \n",
                        inFileName, (unsigned)(benchedSize>>20));
        }   }

        /* Alloc */
        {   void* const origBuff = malloc(benchedSize);
            if (!origBuff) { DISPLAY("\nError: not enough memory!\n"); fclose(inFile); return 12; }

            /* Fill input buffer */
            DISPLAY("Loading %s...       \r", inFileName);
            {   size_t const readSize = fread(origBuff, 1, benchedSize, inFile);
                fclose(inFile);
                if (readSize != benchedSize) {
                    DISPLAY("\nError: problem reading file '%s' !!    \n", inFileName);
                    free(origBuff);
                    return 13;
            }   }

            /* bench */
            DISPLAY("\r%70s\r", "");   /* blank line */
            DISPLAY(" %s : \n", inFileName);
            if (benchNb) {
                benchMem(benchNb, origBuff, benchedSize, cLevel, cparams);
            } else {
                for (benchNb=0; benchNb<100; benchNb++) {
                    benchMem(benchNb, origBuff, benchedSize, cLevel, cparams);
                }
                benchNb = 0;
            }

            free(origBuff);
    }   }

    return 0;
}



/*_*******************************************************
*  Argument Parsing
*********************************************************/

#define ERROR_OUT(msg) { DISPLAY("%s \n", msg); exit(1); }

static unsigned readU32FromChar(const char** stringPtr)
{
    const char errorMsg[] = "error: numeric value too large";
    unsigned result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9')) {
        unsigned const max = (((unsigned)(-1)) / 10) - 1;
        if (result > max) ERROR_OUT(errorMsg);
        result *= 10;
        result += (unsigned)(**stringPtr - '0');
        (*stringPtr)++ ;
    }
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        unsigned const maxK = ((unsigned)(-1)) >> 10;
        if (result > maxK) ERROR_OUT(errorMsg);
        result <<= 10;
        if (**stringPtr=='M') {
            if (result > maxK) ERROR_OUT(errorMsg);
            result <<= 10;
        }
        (*stringPtr)++;  /* skip `K` or `M` */
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    return result;
}

static int longCommandWArg(const char** stringPtr, const char* longCommand)
{
    size_t const comSize = strlen(longCommand);
    int const result = !strncmp(*stringPtr, longCommand, comSize);
    if (result) *stringPtr += comSize;
    return result;
}


/*_*******************************************************
*  Command line
*********************************************************/

static int usage(const char* exename)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [arg] file1 file2 ... fileX\n", exename);
    DISPLAY( "Arguments :\n");
    DISPLAY( " -H/-h  : Help (this text + advanced options)\n");
    return 0;
}

static int usage_advanced(const char* exename)
{
    usage(exename);
    DISPLAY( "\nAdvanced options :\n");
    DISPLAY( " -b#    : test only function # \n");
    DISPLAY( " -l#    : benchmark functions at that compression level (default : %i)\n", DEFAULT_CLEVEL);
    DISPLAY( "--zstd= : custom parameter selection. Format same as zstdcli \n");
    DISPLAY( " -P#    : sample compressibility (default : %.1f%%)\n", COMPRESSIBILITY_DEFAULT * 100);
    DISPLAY( " -B#    : sample size (default : %u)\n", (unsigned)kSampleSizeDefault);
    DISPLAY( " -i#    : iteration loops [1-9](default : %i)\n", NBLOOPS);
    return 0;
}

static int badusage(const char* exename)
{
    DISPLAY("Wrong parameters\n");
    usage(exename);
    return 1;
}

int main(int argc, const char** argv)
{
    int argNb, filenamesStart=0, result;
    const char* const exename = argv[0];
    const char* input_filename = NULL;
    U32 benchNb = 0, main_pause = 0;
    int cLevel = DEFAULT_CLEVEL;
    ZSTD_compressionParameters cparams = ZSTD_getCParams(cLevel, 0, 0);
    size_t sampleSize = kSampleSizeDefault;
    double compressibility = COMPRESSIBILITY_DEFAULT;

    DISPLAY(WELCOME_MESSAGE);
    if (argc<1) return badusage(exename);

    for (argNb=1; argNb<argc; argNb++) {
        const char* argument = argv[argNb];
        CONTROL(argument != NULL);

        if (longCommandWArg(&argument, "--zstd=")) {
            for ( ; ;) {
                if (longCommandWArg(&argument, "windowLog=") || longCommandWArg(&argument, "wlog=")) { cparams.windowLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "chainLog=") || longCommandWArg(&argument, "clog=")) { cparams.chainLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "hashLog=") || longCommandWArg(&argument, "hlog=")) { cparams.hashLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "searchLog=") || longCommandWArg(&argument, "slog=")) { cparams.searchLog = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "minMatch=") || longCommandWArg(&argument, "mml=")) { cparams.minMatch = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "targetLength=") || longCommandWArg(&argument, "tlen=")) { cparams.targetLength = readU32FromChar(&argument); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "strategy=") || longCommandWArg(&argument, "strat=")) { cparams.strategy = (ZSTD_strategy)(readU32FromChar(&argument)); if (argument[0]==',') { argument++; continue; } else break; }
                if (longCommandWArg(&argument, "level=") || longCommandWArg(&argument, "lvl=")) { cLevel = (int)readU32FromChar(&argument); cparams = ZSTD_getCParams(cLevel, 0, 0); if (argument[0]==',') { argument++; continue; } else break; }
                DISPLAY("invalid compression parameter \n");
                return 1;
            }

            /* check end of string */
            if (argument[0] != 0) {
                DISPLAY("invalid --zstd= format \n");
                return 1;
            } else {
                continue;
            }

        } else if (argument[0]=='-') { /* Commands (note : aggregated commands are allowed) */
            argument++;
            while (argument[0]!=0) {

                switch(argument[0])
                {
                    /* Display help on usage */
                case 'h':
                case 'H': return usage_advanced(exename);

                    /* Pause at the end (hidden option) */
                case 'p': main_pause = 1; break;

                    /* Select specific algorithm to bench */
                case 'b':
                    argument++;
                    benchNb = readU32FromChar(&argument);
                    break;

                    /* Select compression level to use */
                case 'l':
                    argument++;
                    cLevel = (int)readU32FromChar(&argument);
                    cparams = ZSTD_getCParams(cLevel, 0, 0);
                    break;

                    /* Select compressibility of synthetic sample */
                case 'P':
                    argument++;
                    compressibility = (double)readU32FromChar(&argument) / 100.;
                    break;

                    /* Select size of synthetic sample */
                case 'B':
                    argument++;
                    sampleSize = (size_t)readU32FromChar(&argument);
                    break;

                    /* Modify Nb Iterations */
                case 'i':
                    argument++;
                    g_nbIterations = readU32FromChar(&argument);
                    break;

                    /* Unknown command */
                default : return badusage(exename);
                }
            }
            continue;
        }

        /* first provided filename is input */
        if (!input_filename) { input_filename=argument; filenamesStart=argNb; continue; }
    }



    if (filenamesStart==0)   /* no input file */
        result = benchSample(benchNb, sampleSize, compressibility, cLevel, cparams);
    else
        result = benchFiles(benchNb, argv+filenamesStart, argc-filenamesStart, cLevel, cparams);

    if (main_pause) { int unused; printf("press enter...\n"); unused = getchar(); (void)unused; }

    return result;
}
