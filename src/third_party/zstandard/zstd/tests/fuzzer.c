/*
 * Copyright (c) Yann Collet, Facebook, Inc.
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
#  pragma warning(disable : 4204)   /* disable: C4204: non-constant aggregate initializer */
#endif


/*-************************************
*  Includes
**************************************/
#include <stdlib.h>       /* free */
#include <stdio.h>        /* fgets, sscanf */
#include <string.h>       /* strcmp */
#undef NDEBUG
#include <assert.h>
#define ZSTD_STATIC_LINKING_ONLY  /* ZSTD_compressContinue, ZSTD_compressBlock */
#include "debug.h"        /* DEBUG_STATIC_ASSERT */
#include "fse.h"
#define ZSTD_DISABLE_DEPRECATE_WARNINGS /* No deprecation warnings, we still test some deprecated functions */
#include "zstd.h"         /* ZSTD_VERSION_STRING */
#include "zstd_errors.h"  /* ZSTD_getErrorCode */
#define ZDICT_STATIC_LINKING_ONLY
#include "zdict.h"        /* ZDICT_trainFromBuffer */
#include "mem.h"
#include "datagen.h"      /* RDG_genBuffer */
#define XXH_STATIC_LINKING_ONLY   /* XXH64_state_t */
#include "xxhash.h"       /* XXH64 */
#include "util.h"
#include "timefn.h"       /* SEC_TO_MICRO, UTIL_time_t, UTIL_TIME_INITIALIZER, UTIL_clockSpanMicro, UTIL_getTime */
/* must be included after util.h, due to ERROR macro redefinition issue on Visual Studio */
#include "zstd_internal.h" /* ZSTD_WORKSPACETOOLARGE_MAXDURATION, ZSTD_WORKSPACETOOLARGE_FACTOR, KB, MB */
#include "threading.h"    /* ZSTD_pthread_create, ZSTD_pthread_join */


/*-************************************
*  Constants
**************************************/
#define GB *(1U<<30)

static const int FUZ_compressibility_default = 50;
static const int nbTestsDefault = 30000;


/*-************************************
*  Display Macros
**************************************/
#define DISPLAY(...)          fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...)  if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); }
static U32 g_displayLevel = 2;

static const U64 g_refreshRate = SEC_TO_MICRO / 6;
static UTIL_time_t g_displayClock = UTIL_TIME_INITIALIZER;

#define DISPLAYUPDATE(l, ...) \
    if (g_displayLevel>=l) { \
        if ((UTIL_clockSpanMicro(g_displayClock) > g_refreshRate) || (g_displayLevel>=4)) \
        { g_displayClock = UTIL_getTime(); DISPLAY(__VA_ARGS__); \
        if (g_displayLevel>=4) fflush(stderr); } \
    }


/*-*******************************************************
*  Compile time test
*********************************************************/
#undef MIN
#undef MAX
/* Declaring the function, to avoid -Wmissing-prototype */
void FUZ_bug976(void);
void FUZ_bug976(void)
{   /* these constants shall not depend on MIN() macro */
    assert(ZSTD_HASHLOG_MAX < 31);
    assert(ZSTD_CHAINLOG_MAX < 31);
}


/*-*******************************************************
*  Internal functions
*********************************************************/
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

#define FUZ_rotl32(x,r) ((x << r) | (x >> (32 - r)))
static U32 FUZ_rand(U32* src)
{
    static const U32 prime1 = 2654435761U;
    static const U32 prime2 = 2246822519U;
    U32 rand32 = *src;
    rand32 *= prime1;
    rand32 += prime2;
    rand32  = FUZ_rotl32(rand32, 13);
    *src = rand32;
    return rand32 >> 5;
}

static U32 FUZ_highbit32(U32 v32)
{
    unsigned nbBits = 0;
    if (v32==0) return 0;
    while (v32) v32 >>= 1, nbBits++;
    return nbBits;
}


/*=============================================
*   Test macros
=============================================*/
#define CHECK_Z(f) {                               \
    size_t const err = f;                          \
    if (ZSTD_isError(err)) {                       \
        DISPLAY("Error => %s : %s ",               \
                #f, ZSTD_getErrorName(err));       \
        exit(1);                                   \
}   }

#define CHECK_VAR(var, fn)  var = fn; if (ZSTD_isError(var)) { DISPLAYLEVEL(1, "%s : fails : %s \n", #fn, ZSTD_getErrorName(var)); goto _output_error; }
#define CHECK_NEWV(var, fn)  size_t const CHECK_VAR(var, fn)
#define CHECK(fn)  { CHECK_NEWV(__err, fn); }
#define CHECKPLUS(var, fn, more)  { CHECK_NEWV(var, fn); more; }

#define CHECK_OP(op, lhs, rhs) {                                  \
    if (!((lhs) op (rhs))) {                                      \
        DISPLAY("Error L%u => FAILED %s %s %s ", __LINE__, #lhs, #op, #rhs);  \
        goto _output_error;                                       \
    }                                                             \
}
#define CHECK_EQ(lhs, rhs) CHECK_OP(==, lhs, rhs)
#define CHECK_LT(lhs, rhs) CHECK_OP(<, lhs, rhs)


/*=============================================
*   Memory Tests
=============================================*/
#if defined(__APPLE__) && defined(__MACH__)

#include <malloc/malloc.h>    /* malloc_size */

typedef struct {
    unsigned long long totalMalloc;
    size_t currentMalloc;
    size_t peakMalloc;
    unsigned nbMalloc;
    unsigned nbFree;
} mallocCounter_t;

static const mallocCounter_t INIT_MALLOC_COUNTER = { 0, 0, 0, 0, 0 };

static void* FUZ_mallocDebug(void* counter, size_t size)
{
    mallocCounter_t* const mcPtr = (mallocCounter_t*)counter;
    void* const ptr = malloc(size);
    if (ptr==NULL) return NULL;
    DISPLAYLEVEL(4, "allocating %u KB => effectively %u KB \n",
        (unsigned)(size >> 10), (unsigned)(malloc_size(ptr) >> 10));  /* OS-X specific */
    mcPtr->totalMalloc += size;
    mcPtr->currentMalloc += size;
    if (mcPtr->currentMalloc > mcPtr->peakMalloc)
        mcPtr->peakMalloc = mcPtr->currentMalloc;
    mcPtr->nbMalloc += 1;
    return ptr;
}

static void FUZ_freeDebug(void* counter, void* address)
{
    mallocCounter_t* const mcPtr = (mallocCounter_t*)counter;
    DISPLAYLEVEL(4, "freeing %u KB \n", (unsigned)(malloc_size(address) >> 10));
    mcPtr->nbFree += 1;
    mcPtr->currentMalloc -= malloc_size(address);  /* OS-X specific */
    free(address);
}

static void FUZ_displayMallocStats(mallocCounter_t count)
{
    DISPLAYLEVEL(3, "peak:%6u KB,  nbMallocs:%2u, total:%6u KB \n",
        (unsigned)(count.peakMalloc >> 10),
        count.nbMalloc,
        (unsigned)(count.totalMalloc >> 10));
}

static int FUZ_mallocTests_internal(unsigned seed, double compressibility, unsigned part,
                void* inBuffer, size_t inSize, void* outBuffer, size_t outSize)
{
    /* test only played in verbose mode, as they are long */
    if (g_displayLevel<3) return 0;

    /* Create compressible noise */
    if (!inBuffer || !outBuffer) {
        DISPLAY("Not enough memory, aborting\n");
        exit(1);
    }
    RDG_genBuffer(inBuffer, inSize, compressibility, 0. /*auto*/, seed);

    /* simple compression tests */
    if (part <= 1)
    {   int compressionLevel;
        for (compressionLevel=1; compressionLevel<=6; compressionLevel++) {
            mallocCounter_t malcount = INIT_MALLOC_COUNTER;
            ZSTD_customMem const cMem = { FUZ_mallocDebug, FUZ_freeDebug, &malcount };
            ZSTD_CCtx* const cctx = ZSTD_createCCtx_advanced(cMem);
            CHECK_Z( ZSTD_compressCCtx(cctx, outBuffer, outSize, inBuffer, inSize, compressionLevel) );
            ZSTD_freeCCtx(cctx);
            DISPLAYLEVEL(3, "compressCCtx level %i : ", compressionLevel);
            FUZ_displayMallocStats(malcount);
    }   }

    /* streaming compression tests */
    if (part <= 2)
    {   int compressionLevel;
        for (compressionLevel=1; compressionLevel<=6; compressionLevel++) {
            mallocCounter_t malcount = INIT_MALLOC_COUNTER;
            ZSTD_customMem const cMem = { FUZ_mallocDebug, FUZ_freeDebug, &malcount };
            ZSTD_CCtx* const cstream = ZSTD_createCStream_advanced(cMem);
            ZSTD_outBuffer out = { outBuffer, outSize, 0 };
            ZSTD_inBuffer in = { inBuffer, inSize, 0 };
            CHECK_Z( ZSTD_initCStream(cstream, compressionLevel) );
            CHECK_Z( ZSTD_compressStream(cstream, &out, &in) );
            CHECK_Z( ZSTD_endStream(cstream, &out) );
            ZSTD_freeCStream(cstream);
            DISPLAYLEVEL(3, "compressStream level %i : ", compressionLevel);
            FUZ_displayMallocStats(malcount);
    }   }

    /* advanced MT API test */
    if (part <= 3)
    {   int nbThreads;
        for (nbThreads=1; nbThreads<=4; nbThreads++) {
            int compressionLevel;
            for (compressionLevel=1; compressionLevel<=6; compressionLevel++) {
                mallocCounter_t malcount = INIT_MALLOC_COUNTER;
                ZSTD_customMem const cMem = { FUZ_mallocDebug, FUZ_freeDebug, &malcount };
                ZSTD_CCtx* const cctx = ZSTD_createCCtx_advanced(cMem);
                CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, compressionLevel) );
                CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, nbThreads) );
                CHECK_Z( ZSTD_compress2(cctx, outBuffer, outSize, inBuffer, inSize) );
                ZSTD_freeCCtx(cctx);
                DISPLAYLEVEL(3, "compress_generic,-T%i,end level %i : ",
                                nbThreads, compressionLevel);
                FUZ_displayMallocStats(malcount);
    }   }   }

    /* advanced MT streaming API test */
    if (part <= 4)
    {   int nbThreads;
        for (nbThreads=1; nbThreads<=4; nbThreads++) {
            int compressionLevel;
            for (compressionLevel=1; compressionLevel<=6; compressionLevel++) {
                mallocCounter_t malcount = INIT_MALLOC_COUNTER;
                ZSTD_customMem const cMem = { FUZ_mallocDebug, FUZ_freeDebug, &malcount };
                ZSTD_CCtx* const cctx = ZSTD_createCCtx_advanced(cMem);
                ZSTD_outBuffer out = { outBuffer, outSize, 0 };
                ZSTD_inBuffer in = { inBuffer, inSize, 0 };
                CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, compressionLevel) );
                CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, nbThreads) );
                CHECK_Z( ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue) );
                while ( ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end) ) {}
                ZSTD_freeCCtx(cctx);
                DISPLAYLEVEL(3, "compress_generic,-T%i,continue level %i : ",
                                nbThreads, compressionLevel);
                FUZ_displayMallocStats(malcount);
    }   }   }

    return 0;
}

static int FUZ_mallocTests(unsigned seed, double compressibility, unsigned part)
{
    size_t const inSize = 64 MB + 16 MB + 4 MB + 1 MB + 256 KB + 64 KB; /* 85.3 MB */
    size_t const outSize = ZSTD_compressBound(inSize);
    void* const inBuffer = malloc(inSize);
    void* const outBuffer = malloc(outSize);
    int result;

    /* Create compressible noise */
    if (!inBuffer || !outBuffer) {
        DISPLAY("Not enough memory, aborting \n");
        exit(1);
    }

    result = FUZ_mallocTests_internal(seed, compressibility, part,
                    inBuffer, inSize, outBuffer, outSize);

    free(inBuffer);
    free(outBuffer);
    return result;
}

#else

static int FUZ_mallocTests(unsigned seed, double compressibility, unsigned part)
{
    (void)seed; (void)compressibility; (void)part;
    return 0;
}

#endif

static void FUZ_decodeSequences(BYTE* dst, ZSTD_Sequence* seqs, size_t seqsSize,
                                BYTE* src, size_t size, ZSTD_sequenceFormat_e format)
{
    size_t i;
    size_t j;
    for(i = 0; i < seqsSize; ++i) {
        assert(dst + seqs[i].litLength + seqs[i].matchLength <= dst + size);
        assert(src + seqs[i].litLength + seqs[i].matchLength <= src + size);
        if (format == ZSTD_sf_noBlockDelimiters) {
            assert(seqs[i].matchLength != 0 || seqs[i].offset != 0);
        }

        memcpy(dst, src, seqs[i].litLength);
        dst += seqs[i].litLength;
        src += seqs[i].litLength;
        size -= seqs[i].litLength;

        if (seqs[i].offset != 0) {
            for (j = 0; j < seqs[i].matchLength; ++j)
                dst[j] = dst[j - seqs[i].offset];
            dst += seqs[i].matchLength;
            src += seqs[i].matchLength;
            size -= seqs[i].matchLength;
        }
    }
    if (format == ZSTD_sf_noBlockDelimiters) {
        memcpy(dst, src, size);
    }
}

#ifdef ZSTD_MULTITHREAD
typedef struct {
    ZSTD_CCtx* cctx;
    ZSTD_threadPool* pool;
    void* CNBuffer;
    size_t CNBuffSize;
    void* compressedBuffer;
    size_t compressedBufferSize;
    void* decodedBuffer;
    int err;
} threadPoolTests_compressionJob_payload;

static void* threadPoolTests_compressionJob(void* payload) {
    threadPoolTests_compressionJob_payload* args = (threadPoolTests_compressionJob_payload*)payload;
    size_t cSize;
    if (ZSTD_isError(ZSTD_CCtx_refThreadPool(args->cctx, args->pool))) args->err = 1;
    cSize = ZSTD_compress2(args->cctx, args->compressedBuffer, args->compressedBufferSize, args->CNBuffer, args->CNBuffSize);
    if (ZSTD_isError(cSize)) args->err = 1;
    if (ZSTD_isError(ZSTD_decompress(args->decodedBuffer, args->CNBuffSize, args->compressedBuffer, cSize))) args->err = 1;
    return payload;
}

static int threadPoolTests(void) {
    int testResult = 0;
    size_t err;

    size_t const CNBuffSize = 5 MB;
    void* const CNBuffer = malloc(CNBuffSize);
    size_t const compressedBufferSize = ZSTD_compressBound(CNBuffSize);
    void* const compressedBuffer = malloc(compressedBufferSize);
    void* const decodedBuffer = malloc(CNBuffSize);

    size_t const kPoolNumThreads = 8;

    RDG_genBuffer(CNBuffer, CNBuffSize, 0.5, 0.5, 0);

    DISPLAYLEVEL(3, "thread pool test : threadPool re-use roundtrips: ");
    {
        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        ZSTD_threadPool* pool = ZSTD_createThreadPool(kPoolNumThreads);

        size_t nbThreads = 1;
        for (; nbThreads <= kPoolNumThreads; ++nbThreads) {
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, (int)nbThreads);
            err = ZSTD_CCtx_refThreadPool(cctx, pool);
            if (ZSTD_isError(err)) {
                DISPLAYLEVEL(3, "refThreadPool error!\n");
                ZSTD_freeCCtx(cctx);
                goto _output_error;
            }
            err = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
            if (ZSTD_isError(err)) {
                DISPLAYLEVEL(3, "Compression error!\n");
                ZSTD_freeCCtx(cctx);
                goto _output_error;
            }
            err = ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, err);
            if (ZSTD_isError(err)) {
                DISPLAYLEVEL(3, "Decompression error!\n");
                ZSTD_freeCCtx(cctx);
                goto _output_error;
            }
        }

        ZSTD_freeCCtx(cctx);
        ZSTD_freeThreadPool(pool);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "thread pool test : threadPool simultaneous usage: ");
    {
        void* const decodedBuffer2 = malloc(CNBuffSize);
        void* const compressedBuffer2 = malloc(compressedBufferSize);
        ZSTD_threadPool* pool = ZSTD_createThreadPool(kPoolNumThreads);
        ZSTD_CCtx* cctx1 = ZSTD_createCCtx();
        ZSTD_CCtx* cctx2 = ZSTD_createCCtx();

        ZSTD_pthread_t t1;
        ZSTD_pthread_t t2;
        threadPoolTests_compressionJob_payload p1 = {cctx1, pool, CNBuffer, CNBuffSize,
                                                     compressedBuffer, compressedBufferSize, decodedBuffer, 0 /* err */};
        threadPoolTests_compressionJob_payload p2 = {cctx2, pool, CNBuffer, CNBuffSize,
                                                     compressedBuffer2, compressedBufferSize, decodedBuffer2, 0 /* err */};

        ZSTD_CCtx_setParameter(cctx1, ZSTD_c_nbWorkers, 2);
        ZSTD_CCtx_setParameter(cctx2, ZSTD_c_nbWorkers, 2);
        ZSTD_CCtx_refThreadPool(cctx1, pool);
        ZSTD_CCtx_refThreadPool(cctx2, pool);

        ZSTD_pthread_create(&t1, NULL, threadPoolTests_compressionJob, &p1);
        ZSTD_pthread_create(&t2, NULL, threadPoolTests_compressionJob, &p2);
        ZSTD_pthread_join(t1, NULL);
        ZSTD_pthread_join(t2, NULL);

        assert(!memcmp(decodedBuffer, decodedBuffer2, CNBuffSize));
        free(decodedBuffer2);
        free(compressedBuffer2);

        ZSTD_freeThreadPool(pool);
        ZSTD_freeCCtx(cctx1);
        ZSTD_freeCCtx(cctx2);

        if (p1.err || p2.err) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");

_end:
    free(CNBuffer);
    free(compressedBuffer);
    free(decodedBuffer);
    return testResult;

_output_error:
    testResult = 1;
    DISPLAY("Error detected in Unit tests ! \n");
    goto _end;
}
#endif /* ZSTD_MULTITHREAD */

/*=============================================
*   Unit tests
=============================================*/

static int basicUnitTests(U32 const seed, double compressibility)
{
    size_t const CNBuffSize = 5 MB;
    void* const CNBuffer = malloc(CNBuffSize);
    size_t const compressedBufferSize = ZSTD_compressBound(CNBuffSize);
    void* const compressedBuffer = malloc(compressedBufferSize);
    void* const decodedBuffer = malloc(CNBuffSize);
    int testResult = 0;
    unsigned testNb=0;
    size_t cSize;

    /* Create compressible noise */
    if (!CNBuffer || !compressedBuffer || !decodedBuffer) {
        DISPLAY("Not enough memory, aborting\n");
        testResult = 1;
        goto _end;
    }
    RDG_genBuffer(CNBuffer, CNBuffSize, compressibility, 0., seed);

    /* Basic tests */
    DISPLAYLEVEL(3, "test%3u : ZSTD_getErrorName : ", testNb++);
    {   const char* errorString = ZSTD_getErrorName(0);
        DISPLAYLEVEL(3, "OK : %s \n", errorString);
    }

    DISPLAYLEVEL(3, "test%3u : ZSTD_getErrorName with wrong value : ", testNb++);
    {   const char* errorString = ZSTD_getErrorName(499);
        DISPLAYLEVEL(3, "OK : %s \n", errorString);
    }

    DISPLAYLEVEL(3, "test%3u : min compression level : ", testNb++);
    {   int const mcl = ZSTD_minCLevel();
        DISPLAYLEVEL(3, "%i (OK) \n", mcl);
    }

    DISPLAYLEVEL(3, "test%3u : default compression level : ", testNb++);
    {   int const defaultCLevel = ZSTD_defaultCLevel();
        if (defaultCLevel != ZSTD_CLEVEL_DEFAULT) goto _output_error;
        DISPLAYLEVEL(3, "%i (OK) \n", defaultCLevel);
    }

    DISPLAYLEVEL(3, "test%3u : ZSTD_versionNumber : ", testNb++);
    {   unsigned const vn = ZSTD_versionNumber();
        DISPLAYLEVEL(3, "%u (OK) \n", vn);
    }

    DISPLAYLEVEL(3, "test%3u : ZSTD_adjustCParams : ", testNb++);
    {
        ZSTD_compressionParameters params;
        memset(&params, 0, sizeof(params));
        params.windowLog = 10;
        params.hashLog = 19;
        params.chainLog = 19;
        params = ZSTD_adjustCParams(params, 1000, 100000);
        if (params.hashLog != 18) goto _output_error;
        if (params.chainLog != 17) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3u : compress %u bytes : ", testNb++, (unsigned)CNBuffSize);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        if (cctx==NULL) goto _output_error;
        CHECK_VAR(cSize, ZSTD_compressCCtx(cctx,
                            compressedBuffer, compressedBufferSize,
                            CNBuffer, CNBuffSize, 1) );
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : size of cctx for level 1 : ", testNb++);
        {   size_t const cctxSize = ZSTD_sizeof_CCtx(cctx);
            DISPLAYLEVEL(3, "%u bytes \n", (unsigned)cctxSize);
        }
        ZSTD_freeCCtx(cctx);
    }

    DISPLAYLEVEL(3, "test%3i : decompress skippable frame -8 size : ", testNb++);
    {
       char const skippable8[] = "\x50\x2a\x4d\x18\xf8\xff\xff\xff";
       size_t const size = ZSTD_decompress(NULL, 0, skippable8, 8);
       if (!ZSTD_isError(size)) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_getFrameContentSize test : ", testNb++);
    {   unsigned long long const rSize = ZSTD_getFrameContentSize(compressedBuffer, cSize);
        if (rSize != CNBuffSize) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_getDecompressedSize test : ", testNb++);
    {   unsigned long long const rSize = ZSTD_getDecompressedSize(compressedBuffer, cSize);
        if (rSize != CNBuffSize) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_findDecompressedSize test : ", testNb++);
    {   unsigned long long const rSize = ZSTD_findDecompressedSize(compressedBuffer, cSize);
        if (rSize != CNBuffSize) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : tight ZSTD_decompressBound test : ", testNb++);
    {
        unsigned long long bound = ZSTD_decompressBound(compressedBuffer, cSize);
        if (bound != CNBuffSize) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_decompressBound test with invalid srcSize : ", testNb++);
    {
        unsigned long long bound = ZSTD_decompressBound(compressedBuffer, cSize - 1);
        if (bound != ZSTD_CONTENTSIZE_ERROR) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress %u bytes : ", testNb++, (unsigned)CNBuffSize);
    { size_t const r = ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize);
      if (r != CNBuffSize) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : check decompressed result : ", testNb++);
    {   size_t u;
        for (u=0; u<CNBuffSize; u++) {
            if (((BYTE*)decodedBuffer)[u] != ((BYTE*)CNBuffer)[u]) goto _output_error;
    }   }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3u : invalid endDirective : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_inBuffer inb = { CNBuffer, CNBuffSize, 0 };
        ZSTD_outBuffer outb = { compressedBuffer, compressedBufferSize, 0 };
        if (cctx==NULL) goto _output_error;
        CHECK( ZSTD_isError( ZSTD_compressStream2(cctx, &outb, &inb, (ZSTD_EndDirective) 3) ) );  /* must fail */
        CHECK( ZSTD_isError( ZSTD_compressStream2(cctx, &outb, &inb, (ZSTD_EndDirective)-1) ) );  /* must fail */
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_checkCParams : ", testNb++);
    {
        ZSTD_parameters params = ZSTD_getParams(3, 0, 0);
        assert(!ZSTD_checkCParams(params.cParams));
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_createDCtx_advanced and ZSTD_sizeof_DCtx: ", testNb++);
    {
        ZSTD_DCtx* const dctx = ZSTD_createDCtx_advanced(ZSTD_defaultCMem);
        assert(dctx != NULL);
        assert(ZSTD_sizeof_DCtx(dctx) != 0);
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : misc unaccounted for zstd symbols : ", testNb++);
    {
        /* %p takes a void*. In ISO C, it's illegal to cast a function pointer
         * to a data pointer. (Although in POSIX you're required to be allowed
         * to do it...) So we have to fall back to our trusty friend memcpy. */
        unsigned (* const funcptr_getDictID)(const ZSTD_DDict* ddict) =
            ZSTD_getDictID_fromDDict;
        ZSTD_DStream* (* const funcptr_createDStream)(
            ZSTD_customMem customMem) = ZSTD_createDStream_advanced;
        void (* const funcptr_copyDCtx)(
            ZSTD_DCtx* dctx, const ZSTD_DCtx* preparedDCtx) = ZSTD_copyDCtx;
        ZSTD_nextInputType_e (* const funcptr_nextInputType)(ZSTD_DCtx* dctx) =
            ZSTD_nextInputType;
        const void *voidptr_getDictID;
        const void *voidptr_createDStream;
        const void *voidptr_copyDCtx;
        const void *voidptr_nextInputType;
        DEBUG_STATIC_ASSERT(sizeof(funcptr_getDictID) == sizeof(voidptr_getDictID));
        memcpy(
            (void*)&voidptr_getDictID,
            (const void*)&funcptr_getDictID,
            sizeof(void*));
        memcpy(
            (void*)&voidptr_createDStream,
            (const void*)&funcptr_createDStream,
            sizeof(void*));
        memcpy(
            (void*)&voidptr_copyDCtx,
            (const void*)&funcptr_copyDCtx,
            sizeof(void*));
        memcpy(
            (void*)&voidptr_nextInputType,
            (const void*)&funcptr_nextInputType,
            sizeof(void*));
        DISPLAYLEVEL(3, "%p ", voidptr_getDictID);
        DISPLAYLEVEL(3, "%p ", voidptr_createDStream);
        DISPLAYLEVEL(3, "%p ", voidptr_copyDCtx);
        DISPLAYLEVEL(3, "%p ", voidptr_nextInputType);
    }
    DISPLAYLEVEL(3, ": OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress with null dict : ", testNb++);
    {   ZSTD_DCtx* const dctx = ZSTD_createDCtx(); assert(dctx != NULL);
        {   size_t const r = ZSTD_decompress_usingDict(dctx,
                                                    decodedBuffer, CNBuffSize,
                                                    compressedBuffer, cSize,
                                                    NULL, 0);
            if (r != CNBuffSize) goto _output_error;
        }
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress with null DDict : ", testNb++);
    {   ZSTD_DCtx* const dctx = ZSTD_createDCtx(); assert(dctx != NULL);
        {   size_t const r = ZSTD_decompress_usingDDict(dctx,
                                                    decodedBuffer, CNBuffSize,
                                                    compressedBuffer, cSize,
                                                    NULL);
            if (r != CNBuffSize) goto _output_error;
        }
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress with 1 missing byte : ", testNb++);
    { size_t const r = ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize-1);
      if (!ZSTD_isError(r)) goto _output_error;
      if (ZSTD_getErrorCode((size_t)r) != ZSTD_error_srcSize_wrong) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress with 1 too much byte : ", testNb++);
    { size_t const r = ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize+1);
      if (!ZSTD_isError(r)) goto _output_error;
      if (ZSTD_getErrorCode(r) != ZSTD_error_srcSize_wrong) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress too large input : ", testNb++);
    { size_t const r = ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, compressedBufferSize);
      if (!ZSTD_isError(r)) goto _output_error;
      if (ZSTD_getErrorCode(r) != ZSTD_error_srcSize_wrong) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress into NULL buffer : ", testNb++);
    { size_t const r = ZSTD_decompress(NULL, 0, compressedBuffer, compressedBufferSize);
      if (!ZSTD_isError(r)) goto _output_error;
      if (ZSTD_getErrorCode(r) != ZSTD_error_dstSize_tooSmall) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress with corrupted checksum : ", testNb++);
    {   /* create compressed buffer with checksumming enabled */
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        if (!cctx) {
            DISPLAY("Not enough memory, aborting\n");
            testResult = 1;
            goto _end;
        }
        CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1) );
        CHECK_VAR(cSize, ZSTD_compress2(cctx,
                            compressedBuffer, compressedBufferSize,
                            CNBuffer, CNBuffSize) );
        ZSTD_freeCCtx(cctx);
    }
    {   /* copy the compressed buffer and corrupt the checksum */
        size_t r;
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        if (!dctx) {
            DISPLAY("Not enough memory, aborting\n");
            testResult = 1;
            goto _end;
        }

        ((char*)compressedBuffer)[cSize-1] += 1;
        r = ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize);
        if (!ZSTD_isError(r)) goto _output_error;
        if (ZSTD_getErrorCode(r) != ZSTD_error_checksum_wrong) goto _output_error;

        CHECK_Z(ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, ZSTD_d_ignoreChecksum));
        r = ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize-1);
        if (!ZSTD_isError(r)) goto _output_error;   /* wrong checksum size should still throw error */
        r = ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize);
        if (ZSTD_isError(r)) goto _output_error;

        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");


    DISPLAYLEVEL(3, "test%3i : ZSTD_decompressBound test with content size missing : ", testNb++);
    {   /* create compressed buffer with content size missing */
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0) );
        CHECK_VAR(cSize, ZSTD_compress2(cctx,
                            compressedBuffer, compressedBufferSize,
                            CNBuffer, CNBuffSize) );
        ZSTD_freeCCtx(cctx);
    }
    {   /* ensure frame content size is missing */
        ZSTD_frameHeader zfh;
        size_t const ret = ZSTD_getFrameHeader(&zfh, compressedBuffer, compressedBufferSize);
        if (ret != 0 || zfh.frameContentSize !=  ZSTD_CONTENTSIZE_UNKNOWN) goto _output_error;
    }
    {   /* ensure CNBuffSize <= decompressBound */
        unsigned long long const bound = ZSTD_decompressBound(compressedBuffer, compressedBufferSize);
        if (CNBuffSize > bound) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d: check DCtx size is reduced after many oversized calls : ", testNb++);
    {
        size_t const largeFrameSrcSize = 200;
        size_t const smallFrameSrcSize = 10;
        size_t const nbFrames = 256;

        size_t i = 0, consumed = 0, produced = 0, prevDCtxSize = 0;
        int sizeReduced = 0;

        BYTE* const dst = (BYTE*)compressedBuffer;
        ZSTD_DCtx* dctx = ZSTD_createDCtx();

        /* create a large frame and then a bunch of small frames */
        size_t srcSize = ZSTD_compress((void*)dst,
            compressedBufferSize, CNBuffer, largeFrameSrcSize, 3);
        for (i = 0; i < nbFrames; i++)
            srcSize += ZSTD_compress((void*)(dst + srcSize),
                compressedBufferSize - srcSize, CNBuffer,
                smallFrameSrcSize, 3);

        /* decompressStream and make sure that dctx size was reduced at least once */
        while (consumed < srcSize) {
            ZSTD_inBuffer in = {(void*)(dst + consumed), MIN(1, srcSize - consumed), 0};
            ZSTD_outBuffer out = {(BYTE*)CNBuffer + produced, CNBuffSize - produced, 0};
            ZSTD_decompressStream(dctx, &out, &in);
            consumed += in.pos;
            produced += out.pos;

            /* success! size was reduced from the previous frame */
            if (prevDCtxSize > ZSTD_sizeof_DCtx(dctx))
                sizeReduced = 1;

            prevDCtxSize = ZSTD_sizeof_DCtx(dctx);
        }

        assert(sizeReduced);

        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    {
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_CDict* const cdict = ZSTD_createCDict(CNBuffer, 100, 1);
        ZSTD_parameters const params = ZSTD_getParams(1, 0, 0);
        CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_format, ZSTD_f_zstd1_magicless) );

        DISPLAYLEVEL(3, "test%3i : ZSTD_compressCCtx() doesn't use advanced parameters", testNb++);
        CHECK_Z(ZSTD_compressCCtx(cctx, compressedBuffer, compressedBufferSize, NULL, 0, 1));
        if (MEM_readLE32(compressedBuffer) != ZSTD_MAGICNUMBER) goto _output_error;
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compress_usingDict() doesn't use advanced parameters: ", testNb++);
        CHECK_Z(ZSTD_compress_usingDict(cctx, compressedBuffer, compressedBufferSize, NULL, 0, NULL, 0, 1));
        if (MEM_readLE32(compressedBuffer) != ZSTD_MAGICNUMBER) goto _output_error;
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compress_usingCDict() doesn't use advanced parameters: ", testNb++);
        CHECK_Z(ZSTD_compress_usingCDict(cctx, compressedBuffer, compressedBufferSize, NULL, 0, cdict));
        if (MEM_readLE32(compressedBuffer) != ZSTD_MAGICNUMBER) goto _output_error;
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compress_advanced() doesn't use advanced parameters: ", testNb++);
        CHECK_Z(ZSTD_compress_advanced(cctx, compressedBuffer, compressedBufferSize, NULL, 0, NULL, 0, params));
        if (MEM_readLE32(compressedBuffer) != ZSTD_MAGICNUMBER) goto _output_error;
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compress_usingCDict_advanced() doesn't use advanced parameters: ", testNb++);
        CHECK_Z(ZSTD_compress_usingCDict_advanced(cctx, compressedBuffer, compressedBufferSize, NULL, 0, cdict, params.fParams));
        if (MEM_readLE32(compressedBuffer) != ZSTD_MAGICNUMBER) goto _output_error;
        DISPLAYLEVEL(3, "OK \n");

        ZSTD_freeCDict(cdict);
        ZSTD_freeCCtx(cctx);
    }

    DISPLAYLEVEL(3, "test%3i : ldm fill dict out-of-bounds check", testNb++);
    {
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();

        size_t const size = (1U << 10);
        size_t const dstCapacity = ZSTD_compressBound(size);
        void* dict = (void*)malloc(size);
        void* src = (void*)malloc(size);
        void* dst = (void*)malloc(dstCapacity);

        RDG_genBuffer(dict, size, 0.5, 0.5, seed);
        RDG_genBuffer(src, size, 0.5, 0.5, seed);

        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1));
        assert(!ZSTD_isError(ZSTD_compress_usingDict(cctx, dst, dstCapacity, src, size, dict, size, 3)));

        ZSTD_freeCCtx(cctx);
        free(dict);
        free(src);
        free(dst);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : testing dict compression with enableLdm and forceMaxWindow : ", testNb++);
    {
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        void* dict = (void*)malloc(CNBuffSize);
        int nbWorkers;

        for (nbWorkers = 0; nbWorkers < 3; ++nbWorkers) {
            RDG_genBuffer(dict, CNBuffSize, 0.5, 0.5, seed);
            RDG_genBuffer(CNBuffer, CNBuffSize, 0.6, 0.6, seed);

            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, nbWorkers));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_forceMaxWindow, 1));
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1));
            CHECK_Z(ZSTD_CCtx_refPrefix(cctx, dict, CNBuffSize));
            cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
            CHECK_Z(cSize);
            CHECK_Z(ZSTD_decompress_usingDict(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize, dict, CNBuffSize));
        }

        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
        free(dict);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : testing dict compression for determinism : ", testNb++);
    {
        size_t const testSize = 1024;
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        char* dict = (char*)malloc(2 * testSize);
        int ldmEnabled, level;

        RDG_genBuffer(dict, testSize, 0.5, 0.5, seed);
        RDG_genBuffer(CNBuffer, testSize, 0.6, 0.6, seed);
        memcpy(dict + testSize, CNBuffer, testSize);
        for (level = 1; level <= 5; ++level) {
            for (ldmEnabled = 0; ldmEnabled <= 1; ++ldmEnabled) {
                size_t cSize0;
                XXH64_hash_t compressedChecksum0;

                CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
                CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level));
                CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, ldmEnabled));
                CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_deterministicRefPrefix, 1));

                CHECK_Z(ZSTD_CCtx_refPrefix(cctx, dict, testSize));
                cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, testSize);
                CHECK_Z(cSize);
                CHECK_Z(ZSTD_decompress_usingDict(dctx, decodedBuffer, testSize, compressedBuffer, cSize, dict, testSize));

                cSize0 = cSize;
                compressedChecksum0 = XXH64(compressedBuffer, cSize, 0);

                CHECK_Z(ZSTD_CCtx_refPrefix(cctx, dict, testSize));
                cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, dict + testSize, testSize);
                CHECK_Z(cSize);

                if (cSize != cSize0) goto _output_error;
                if (XXH64(compressedBuffer, cSize, 0) != compressedChecksum0) goto _output_error;
            }
        }

        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
        free(dict);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : LDM + opt parser with small uncompressible block ", testNb++);
    {   ZSTD_CCtx* cctx = ZSTD_createCCtx();
        ZSTD_DCtx* dctx = ZSTD_createDCtx();
        size_t const srcSize = 300 KB;
        size_t const flushSize = 128 KB + 5;
        size_t const dstSize = ZSTD_compressBound(srcSize);
        char* src = (char*)CNBuffer;
        char* dst = (char*)compressedBuffer;

        ZSTD_outBuffer out = { dst, dstSize, 0 };
        ZSTD_inBuffer in = { src, flushSize, 0 };

        if (!cctx || !dctx) {
            DISPLAY("Not enough memory, aborting\n");
            testResult = 1;
            goto _end;
        }

        RDG_genBuffer(src, srcSize, 0.5, 0.5, seed);
        /* Force an LDM to exist that crosses block boundary into uncompressible block */
        memcpy(src + 125 KB, src, 3 KB + 5);

        /* Enable MT, LDM, and opt parser */
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 19));

        /* Flushes a block of 128 KB and block of 5 bytes */
        CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush));

        /* Compress the rest */
        in.size = 300 KB;
        CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));

        CHECK_Z(ZSTD_decompress(decodedBuffer, CNBuffSize, dst, out.pos));

        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : testing ldm dictionary gets invalidated : ", testNb++);
    {
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        void* dict = (void*)malloc(CNBuffSize);
        size_t const kWindowLog = 10;
        size_t const kWindowSize = (size_t)1 << kWindowLog;
        size_t const dictSize = kWindowSize * 10;
        size_t const srcSize1 = kWindowSize / 2;
        size_t const srcSize2 = kWindowSize * 10;

        if (CNBuffSize < dictSize) goto _output_error;

        RDG_genBuffer(dict, dictSize, 0.5, 0.5, seed);
        RDG_genBuffer(CNBuffer, srcSize1 + srcSize2, 0.5, 0.5, seed);

        /* Enable checksum to verify round trip. */
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
        /* Disable content size to skip single-pass decompression. */
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, (int)kWindowLog));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_ldmMinMatch, 32));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_ldmHashRateLog, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_ldmHashLog, 16));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_ldmBucketSizeLog, 3));

        /* Round trip once with a dictionary. */
        CHECK_Z(ZSTD_CCtx_refPrefix(cctx, dict, dictSize));
        cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, srcSize1);
        CHECK_Z(cSize);
        CHECK_Z(ZSTD_decompress_usingDict(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize, dict, dictSize));
        cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, srcSize2);
        /* Streaming decompression to catch out of bounds offsets. */
        {
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            ZSTD_outBuffer out = {decodedBuffer, CNBuffSize, 0};
            size_t const dSize = ZSTD_decompressStream(dctx, &out, &in);
            CHECK_Z(dSize);
            if (dSize != 0) goto _output_error;
        }

        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 2));
        /* Round trip once with a dictionary. */
        CHECK_Z(ZSTD_CCtx_refPrefix(cctx, dict, dictSize));
        {
            ZSTD_inBuffer in = {CNBuffer, srcSize1, 0};
            ZSTD_outBuffer out = {compressedBuffer, compressedBufferSize, 0};
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush));
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));
            cSize = out.pos;
        }
        CHECK_Z(ZSTD_decompress_usingDict(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize, dict, dictSize));
        {
            ZSTD_inBuffer in = {CNBuffer, srcSize2, 0};
            ZSTD_outBuffer out = {compressedBuffer, compressedBufferSize, 0};
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush));
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));
            cSize = out.pos;
        }
        /* Streaming decompression to catch out of bounds offsets. */
        {
            ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
            ZSTD_outBuffer out = {decodedBuffer, CNBuffSize, 0};
            size_t const dSize = ZSTD_decompressStream(dctx, &out, &in);
            CHECK_Z(dSize);
            if (dSize != 0) goto _output_error;
        }

        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
        free(dict);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Note: this test takes 0.5 seconds to run */
    DISPLAYLEVEL(3, "test%3i : testing refPrefx vs refPrefx + ldm (size comparison) : ", testNb++);
    {
        /* test a big buffer so that ldm can take effect */
        size_t const size = 100 MB;
        int const windowLog = 27;
        size_t const dstSize = ZSTD_compressBound(size);

        void* dict = (void*)malloc(size);
        void* src = (void*)malloc(size);
        void* dst = (void*)malloc(dstSize);
        void* recon = (void*)malloc(size);

        size_t refPrefixCompressedSize = 0;
        size_t refPrefixLdmComrpessedSize = 0;
        size_t reconSize = 0;

        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();

        /* make dict and src the same uncompressible data */
        RDG_genBuffer(src, size, 0, 0, seed);
        memcpy(dict, src, size);
        assert(!memcmp(dict, src, size));

        /* set level 1 and windowLog to cover src */
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, windowLog));

        /* compress on level 1 using just refPrefix and no ldm */
        ZSTD_CCtx_refPrefix(cctx, dict, size);
        refPrefixCompressedSize = ZSTD_compress2(cctx, dst, dstSize, src, size);
        assert(!ZSTD_isError(refPrefixCompressedSize));

        /* test round trip just refPrefix */
        ZSTD_DCtx_refPrefix(dctx, dict, size);
        reconSize = ZSTD_decompressDCtx(dctx, recon, size, dst, refPrefixCompressedSize);
        assert(!ZSTD_isError(reconSize));
        assert(reconSize == size);
        assert(!memcmp(recon, src, size));

        /* compress on level 1 using refPrefix and ldm */
        ZSTD_CCtx_refPrefix(cctx, dict, size);;
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1))
        refPrefixLdmComrpessedSize = ZSTD_compress2(cctx, dst, dstSize, src, size);
        assert(!ZSTD_isError(refPrefixLdmComrpessedSize));

        /* test round trip refPrefix + ldm*/
        ZSTD_DCtx_refPrefix(dctx, dict, size);
        reconSize = ZSTD_decompressDCtx(dctx, recon, size, dst, refPrefixLdmComrpessedSize);
        assert(!ZSTD_isError(reconSize));
        assert(reconSize == size);
        assert(!memcmp(recon, src, size));

        /* make sure that refPrefixCompressedSize is significantly greater */
        assert(refPrefixCompressedSize > 10 * refPrefixLdmComrpessedSize);
        /* make sure the ldm comrpessed size is less than 1% of original */
        assert((double)refPrefixLdmComrpessedSize / (double)size < 0.01);

        ZSTD_freeDCtx(dctx);
        ZSTD_freeCCtx(cctx);
        free(recon);
        free(dict);
        free(src);
        free(dst);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d: superblock uncompressible data, too many nocompress superblocks : ", testNb++);
    {
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        const BYTE* src = (BYTE*)CNBuffer; BYTE* dst = (BYTE*)compressedBuffer;
        size_t srcSize = 321656; size_t dstCapacity = ZSTD_compressBound(srcSize);

        /* This is the number of bytes to stream before ending. This value
         * was obtained by trial and error :/. */

        const size_t streamCompressThreshold = 161792;
        const size_t streamCompressDelta = 1024;

        /* The first 1/5 of the buffer is compressible and the last 4/5 is
         * uncompressible. This is an approximation of the type of data
         * the fuzzer generated to catch this bug. Streams like this were making
         * zstd generate noCompress superblocks (which are larger than the src
         * they come from). Do this enough times, and we'll run out of room
         * and throw a dstSize_tooSmall error. */

        const size_t compressiblePartSize = srcSize/5;
        const size_t uncompressiblePartSize = srcSize-compressiblePartSize;
        RDG_genBuffer(CNBuffer, compressiblePartSize, 0.5, 0.5, seed);
        RDG_genBuffer((BYTE*)CNBuffer+compressiblePartSize, uncompressiblePartSize, 0, 0, seed);

        /* Setting target block size so that superblock is used */

        assert(cctx != NULL);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_targetCBlockSize, 81);

        {   size_t read;
            for (read = 0; read < streamCompressThreshold; read += streamCompressDelta) {
                ZSTD_inBuffer in = {src, streamCompressDelta, 0};
                ZSTD_outBuffer out = {dst, dstCapacity, 0};
                CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue));
                CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));
                src += streamCompressDelta; srcSize -= streamCompressDelta;
                dst += out.pos; dstCapacity -= out.pos;
        }   }

        /* This is trying to catch a dstSize_tooSmall error */

        {   ZSTD_inBuffer in = {src, srcSize, 0};
            ZSTD_outBuffer out = {dst, dstCapacity, 0};
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));
        }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d: superblock with no literals : ", testNb++);
    /* Generate the same data 20 times over */
    {   size_t const avgChunkSize = CNBuffSize / 20;
        size_t b;
        for (b = 0; b < CNBuffSize; b += avgChunkSize) {
            size_t const chunkSize = MIN(CNBuffSize - b, avgChunkSize);
            RDG_genBuffer((char*)CNBuffer + b, chunkSize, compressibility, 0. /* auto */, seed);
    }   }
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        size_t const normalCSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
        size_t const allowedExpansion = (CNBuffSize * 3 / 1000);
        size_t superCSize;
        CHECK_Z(normalCSize);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 19);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_targetCBlockSize, 1000);
        superCSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
        CHECK_Z(superCSize);
        if (superCSize > normalCSize + allowedExpansion) {
            DISPLAYLEVEL(1, "Superblock too big: %u > %u + %u \n", (U32)superCSize, (U32)normalCSize, (U32)allowedExpansion);
            goto _output_error;
        }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    RDG_genBuffer(CNBuffer, CNBuffSize, compressibility, 0. /*auto*/, seed);
    DISPLAYLEVEL(3, "test%3d: superblock enough room for checksum : ", testNb++)
    /* This tests whether or not we leave enough room for the checksum at the end
     * of the dst buffer. The bug that motivated this test was found by the
     * stream_round_trip fuzzer but this crashes for the same reason and is
     * far more compact than re-creating the stream_round_trip fuzzer's code path */
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_targetCBlockSize, 64);
        assert(!ZSTD_isError(ZSTD_compress2(cctx, compressedBuffer, 1339, CNBuffer, 1278)));
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : compress a NULL input with each level : ", testNb++);
    {   int level = -1;
        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        if (!cctx) goto _output_error;
        for (level = -1; level <= ZSTD_maxCLevel(); ++level) {
          CHECK_Z( ZSTD_compress(compressedBuffer, compressedBufferSize, NULL, 0, level) );
          CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level) );
          CHECK_Z( ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, NULL, 0) );
        }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d : check CCtx size after compressing empty input : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        size_t const r = ZSTD_compressCCtx(cctx, compressedBuffer, compressedBufferSize, NULL, 0, 19);
        if (ZSTD_isError(r)) goto _output_error;
        if (ZSTD_sizeof_CCtx(cctx) > (1U << 20)) goto _output_error;
        ZSTD_freeCCtx(cctx);
        cSize = r;
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d : decompress empty frame into NULL : ", testNb++);
    {   size_t const r = ZSTD_decompress(NULL, 0, compressedBuffer, cSize);
        if (ZSTD_isError(r)) goto _output_error;
        if (r != 0) goto _output_error;
    }
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_outBuffer output;
        if (cctx==NULL) goto _output_error;
        output.dst = compressedBuffer;
        output.size = compressedBufferSize;
        output.pos = 0;
        CHECK_Z( ZSTD_initCStream(cctx, 1) );    /* content size unknown */
        CHECK_Z( ZSTD_flushStream(cctx, &output) );   /* ensure no possibility to "concatenate" and determine the content size */
        CHECK_Z( ZSTD_endStream(cctx, &output) );
        ZSTD_freeCCtx(cctx);
        /* single scan decompression */
        {   size_t const r = ZSTD_decompress(NULL, 0, compressedBuffer, output.pos);
            if (ZSTD_isError(r)) goto _output_error;
            if (r != 0) goto _output_error;
        }
        /* streaming decompression */
        {   ZSTD_DCtx* const dstream = ZSTD_createDStream();
            ZSTD_inBuffer dinput;
            ZSTD_outBuffer doutput;
            size_t ipos;
            if (dstream==NULL) goto _output_error;
            dinput.src = compressedBuffer;
            dinput.size = 0;
            dinput.pos = 0;
            doutput.dst = NULL;
            doutput.size = 0;
            doutput.pos = 0;
            CHECK_Z ( ZSTD_initDStream(dstream) );
            for (ipos=1; ipos<=output.pos; ipos++) {
                dinput.size = ipos;
                CHECK_Z ( ZSTD_decompressStream(dstream, &doutput, &dinput) );
            }
            if (doutput.pos != 0) goto _output_error;
            ZSTD_freeDStream(dstream);
        }
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d : re-use CCtx with expanding block size : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_parameters const params = ZSTD_getParams(1, ZSTD_CONTENTSIZE_UNKNOWN, 0);
        assert(params.fParams.contentSizeFlag == 1);  /* block size will be adapted if pledgedSrcSize is enabled */
        CHECK_Z( ZSTD_compressBegin_advanced(cctx, NULL, 0, params, 1 /*pledgedSrcSize*/) );
        CHECK_Z( ZSTD_compressEnd(cctx, compressedBuffer, compressedBufferSize, CNBuffer, 1) ); /* creates a block size of 1 */

        CHECK_Z( ZSTD_compressBegin_advanced(cctx, NULL, 0, params, ZSTD_CONTENTSIZE_UNKNOWN) );  /* re-use same parameters */
        {   size_t const inSize = 2* 128 KB;
            size_t const outSize = ZSTD_compressBound(inSize);
            CHECK_Z( ZSTD_compressEnd(cctx, compressedBuffer, outSize, CNBuffer, inSize) );
            /* will fail if blockSize is not resized */
        }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d : re-using a CCtx should compress the same : ", testNb++);
    {   size_t const sampleSize = 30;
        int i;
        for (i=0; i<20; i++)
            ((char*)CNBuffer)[i] = (char)i;   /* ensure no match during initial section */
        memcpy((char*)CNBuffer + 20, CNBuffer, 10);   /* create one match, starting from beginning of sample, which is the difficult case (see #1241) */
        for (i=1; i<=19; i++) {
            ZSTD_CCtx* const cctx = ZSTD_createCCtx();
            size_t size1, size2;
            DISPLAYLEVEL(5, "l%i ", i);
            size1 = ZSTD_compressCCtx(cctx, compressedBuffer, compressedBufferSize, CNBuffer, sampleSize, i);
            CHECK_Z(size1);

            size2 = ZSTD_compressCCtx(cctx, compressedBuffer, compressedBufferSize, CNBuffer, sampleSize, i);
            CHECK_Z(size2);
            CHECK_EQ(size1, size2);

            CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, i) );
            size2 = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, sampleSize);
            CHECK_Z(size2);
            CHECK_EQ(size1, size2);

            size2 = ZSTD_compress2(cctx, compressedBuffer, ZSTD_compressBound(sampleSize) - 1, CNBuffer, sampleSize);  /* force streaming, as output buffer is not large enough to guarantee success */
            CHECK_Z(size2);
            CHECK_EQ(size1, size2);

            {   ZSTD_inBuffer inb;
                ZSTD_outBuffer outb;
                inb.src = CNBuffer;
                inb.pos = 0;
                inb.size = sampleSize;
                outb.dst = compressedBuffer;
                outb.pos = 0;
                outb.size = ZSTD_compressBound(sampleSize) - 1;  /* force streaming, as output buffer is not large enough to guarantee success */
                CHECK_Z( ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_end) );
                assert(inb.pos == inb.size);
                CHECK_EQ(size1, outb.pos);
            }

            ZSTD_freeCCtx(cctx);
        }
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d : btultra2 & 1st block : ", testNb++);
    {   size_t const sampleSize = 1024;
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_inBuffer inb;
        ZSTD_outBuffer outb;
        inb.src = CNBuffer;
        inb.pos = 0;
        inb.size = 0;
        outb.dst = compressedBuffer;
        outb.pos = 0;
        outb.size = compressedBufferSize;
        CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, ZSTD_maxCLevel()) );

        inb.size = sampleSize;   /* start with something, so that context is already used */
        CHECK_Z( ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_end) );   /* will break internal assert if stats_init is not disabled */
        assert(inb.pos == inb.size);
        outb.pos = 0;     /* cancel output */

        CHECK_Z( ZSTD_CCtx_setPledgedSrcSize(cctx, sampleSize) );
        inb.size = 4;   /* too small size : compression will be skipped */
        inb.pos = 0;
        CHECK_Z( ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_flush) );
        assert(inb.pos == inb.size);

        inb.size += 5;   /* too small size : compression will be skipped */
        CHECK_Z( ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_flush) );
        assert(inb.pos == inb.size);

        inb.size += 11;   /* small enough to attempt compression */
        CHECK_Z( ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_flush) );
        assert(inb.pos == inb.size);

        assert(inb.pos < sampleSize);
        inb.size = sampleSize;   /* large enough to trigger stats_init, but no longer at beginning */
        CHECK_Z( ZSTD_compressStream2(cctx, &outb, &inb, ZSTD_e_end) );   /* will break internal assert if stats_init is not disabled */
        assert(inb.pos == inb.size);
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d : ZSTD_CCtx_getParameter() : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_outBuffer out = {NULL, 0, 0};
        ZSTD_inBuffer in = {NULL, 0, 0};
        int value;

        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_compressionLevel, &value));
        CHECK_EQ(value, 3);
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_hashLog, &value));
        CHECK_EQ(value, 0);
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_hashLog, ZSTD_HASHLOG_MIN));
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_compressionLevel, &value));
        CHECK_EQ(value, 3);
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_hashLog, &value));
        CHECK_EQ(value, ZSTD_HASHLOG_MIN);
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 7));
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_compressionLevel, &value));
        CHECK_EQ(value, 7);
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_hashLog, &value));
        CHECK_EQ(value, ZSTD_HASHLOG_MIN);
        /* Start a compression job */
        ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue);
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_compressionLevel, &value));
        CHECK_EQ(value, 7);
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_hashLog, &value));
        CHECK_EQ(value, ZSTD_HASHLOG_MIN);
        /* Reset the CCtx */
        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_compressionLevel, &value));
        CHECK_EQ(value, 7);
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_hashLog, &value));
        CHECK_EQ(value, ZSTD_HASHLOG_MIN);
        /* Reset the parameters */
        ZSTD_CCtx_reset(cctx, ZSTD_reset_parameters);
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_compressionLevel, &value));
        CHECK_EQ(value, 3);
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_hashLog, &value));
        CHECK_EQ(value, 0);

        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d : ldm conditionally enabled by default doesn't change cctx params: ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_outBuffer out = {NULL, 0, 0};
        ZSTD_inBuffer in = {NULL, 0, 0};
        int value;

        /* Even if LDM will be enabled by default in the applied params (since wlog >= 27 and strategy >= btopt),
         * we should not modify the actual parameter specified by the user within the CCtx
         */
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 27));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_strategy, ZSTD_btopt));

        CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue));
        CHECK_Z(ZSTD_CCtx_getParameter(cctx, ZSTD_c_enableLongDistanceMatching, &value));
        CHECK_EQ(value, 0);

        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* this test is really too long, and should be made faster */
    DISPLAYLEVEL(3, "test%3d : overflow protection with large windowLog : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_parameters params = ZSTD_getParams(-999, ZSTD_CONTENTSIZE_UNKNOWN, 0);
        size_t const nbCompressions = ((1U << 31) / CNBuffSize) + 2;   /* ensure U32 overflow protection is triggered */
        size_t cnb;
        assert(cctx != NULL);
        params.fParams.contentSizeFlag = 0;
        params.cParams.windowLog = ZSTD_WINDOWLOG_MAX;
        for (cnb = 0; cnb < nbCompressions; ++cnb) {
            DISPLAYLEVEL(6, "run %zu / %zu \n", cnb, nbCompressions);
            CHECK_Z( ZSTD_compressBegin_advanced(cctx, NULL, 0, params, ZSTD_CONTENTSIZE_UNKNOWN) );  /* re-use same parameters */
            CHECK_Z( ZSTD_compressEnd(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize) );
        }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3d : size down context : ", testNb++);
    {   ZSTD_CCtx* const largeCCtx = ZSTD_createCCtx();
        assert(largeCCtx != NULL);
        CHECK_Z( ZSTD_compressBegin(largeCCtx, 19) );   /* streaming implies ZSTD_CONTENTSIZE_UNKNOWN, which maximizes memory usage */
        CHECK_Z( ZSTD_compressEnd(largeCCtx, compressedBuffer, compressedBufferSize, CNBuffer, 1) );
        {   size_t const largeCCtxSize = ZSTD_sizeof_CCtx(largeCCtx);   /* size of context must be measured after compression */
            {   ZSTD_CCtx* const smallCCtx = ZSTD_createCCtx();
                assert(smallCCtx != NULL);
                CHECK_Z(ZSTD_compressCCtx(smallCCtx, compressedBuffer, compressedBufferSize, CNBuffer, 1, 1));
                {   size_t const smallCCtxSize = ZSTD_sizeof_CCtx(smallCCtx);
                    DISPLAYLEVEL(5, "(large) %zuKB > 32*%zuKB (small) : ",
                                largeCCtxSize>>10, smallCCtxSize>>10);
                    assert(largeCCtxSize > 32* smallCCtxSize);  /* note : "too large" definition is handled within zstd_compress.c .
                                                                 * make this test case extreme, so that it doesn't depend on a possibly fluctuating definition */
                }
                ZSTD_freeCCtx(smallCCtx);
            }
            {   U32 const maxNbAttempts = 1100;   /* nb of usages before triggering size down is handled within zstd_compress.c.
                                                   * currently defined as 128x, but could be adjusted in the future.
                                                   * make this test long enough so that it's not too much tied to the current definition within zstd_compress.c */
                unsigned u;
                for (u=0; u<maxNbAttempts; u++) {
                    CHECK_Z(ZSTD_compressCCtx(largeCCtx, compressedBuffer, compressedBufferSize, CNBuffer, 1, 1));
                    if (ZSTD_sizeof_CCtx(largeCCtx) < largeCCtxSize) break;   /* sized down */
                }
                DISPLAYLEVEL(5, "size down after %u attempts : ", u);
                if (u==maxNbAttempts) goto _output_error;   /* no sizedown happened */
            }
        }
        ZSTD_freeCCtx(largeCCtx);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Static CCtx tests */
#define STATIC_CCTX_LEVEL 4
    DISPLAYLEVEL(3, "test%3i : create static CCtx for level %u : ", testNb++, STATIC_CCTX_LEVEL);
    {   size_t const staticCStreamSize = ZSTD_estimateCStreamSize(STATIC_CCTX_LEVEL);
        void* const staticCCtxBuffer = malloc(staticCStreamSize);
        size_t const staticDCtxSize = ZSTD_estimateDCtxSize();
        void* const staticDCtxBuffer = malloc(staticDCtxSize);
        DISPLAYLEVEL(4, "CStream size = %u, ", (U32)staticCStreamSize);
        if (staticCCtxBuffer==NULL || staticDCtxBuffer==NULL) {
            free(staticCCtxBuffer);
            free(staticDCtxBuffer);
            DISPLAY("Not enough memory, aborting\n");
            testResult = 1;
            goto _end;
        }
        {   size_t const smallInSize = 32 KB;
            ZSTD_compressionParameters const cparams_small = ZSTD_getCParams(STATIC_CCTX_LEVEL, smallInSize, 0);
            size_t const smallCCtxSize = ZSTD_estimateCCtxSize_usingCParams(cparams_small);
            size_t const staticCCtxSize = ZSTD_estimateCCtxSize(STATIC_CCTX_LEVEL);
            ZSTD_CCtx* staticCCtx = ZSTD_initStaticCCtx(staticCCtxBuffer, smallCCtxSize);
            ZSTD_DCtx* const staticDCtx = ZSTD_initStaticDCtx(staticDCtxBuffer, staticDCtxSize);
            DISPLAYLEVEL(4, "Full CCtx size = %u, ", (U32)staticCCtxSize);
            DISPLAYLEVEL(4, "CCtx for 32 KB = %u, ", (U32)smallCCtxSize);
            if ((staticCCtx==NULL) || (staticDCtx==NULL)) goto _output_error;
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : compress small input with small static CCtx : ", testNb++);
            CHECK_VAR(cSize, ZSTD_compressCCtx(staticCCtx,
                                  compressedBuffer, compressedBufferSize,
                                  CNBuffer, smallInSize, STATIC_CCTX_LEVEL) );
            DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n",
                            (unsigned)cSize, (double)cSize/smallInSize*100);

            DISPLAYLEVEL(3, "test%3i : compress large input with small static CCtx (must fail) : ", testNb++);
            {   size_t const r = ZSTD_compressCCtx(staticCCtx,
                                  compressedBuffer, compressedBufferSize,
                                  CNBuffer, CNBuffSize, STATIC_CCTX_LEVEL);
                if (ZSTD_getErrorCode((size_t)r) != ZSTD_error_memory_allocation) goto _output_error;
            }
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : resize context to full CCtx size : ", testNb++);
            staticCCtx = ZSTD_initStaticCStream(staticCCtxBuffer, staticCCtxSize);
            DISPLAYLEVEL(4, "staticCCtxBuffer = %p,  staticCCtx = %p , ", staticCCtxBuffer, (void*)staticCCtx);
            if (staticCCtx == NULL) goto _output_error;
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : compress large input with static CCtx : ", testNb++);
            CHECK_VAR(cSize, ZSTD_compressCCtx(staticCCtx,
                                  compressedBuffer, compressedBufferSize,
                                  CNBuffer, CNBuffSize, STATIC_CCTX_LEVEL) );
            DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n",
                            (unsigned)cSize, (double)cSize/CNBuffSize*100);

            DISPLAYLEVEL(3, "test%3i : compress small input often enough to trigger context reduce : ", testNb++);
            {   int nbc;
                assert(staticCCtxSize > smallCCtxSize * ZSTD_WORKSPACETOOLARGE_FACTOR);  /* ensure size down scenario */
                assert(CNBuffSize > smallInSize + ZSTD_WORKSPACETOOLARGE_MAXDURATION + 3);
                for (nbc=0; nbc<ZSTD_WORKSPACETOOLARGE_MAXDURATION+2; nbc++) {
                    CHECK_Z(ZSTD_compressCCtx(staticCCtx,
                                  compressedBuffer, compressedBufferSize,
                                  (char*)CNBuffer + nbc, smallInSize,
                                  STATIC_CCTX_LEVEL) );
            }   }
            DISPLAYLEVEL(3, "OK \n")

            DISPLAYLEVEL(3, "test%3i : init CCtx for level %u : ", testNb++, STATIC_CCTX_LEVEL);
            CHECK_Z( ZSTD_compressBegin(staticCCtx, STATIC_CCTX_LEVEL) );
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : compression again with static CCtx : ", testNb++);
            CHECK_VAR(cSize, ZSTD_compressCCtx(staticCCtx,
                                  compressedBuffer, compressedBufferSize,
                                  CNBuffer, CNBuffSize, STATIC_CCTX_LEVEL) );
            DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n",
                            (unsigned)cSize, (double)cSize/CNBuffSize*100);

            DISPLAYLEVEL(3, "test%3i : simple decompression test with static DCtx : ", testNb++);
            { size_t const r = ZSTD_decompressDCtx(staticDCtx,
                                                decodedBuffer, CNBuffSize,
                                                compressedBuffer, cSize);
              if (r != CNBuffSize) goto _output_error; }
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : check decompressed result : ", testNb++);
            if (memcmp(decodedBuffer, CNBuffer, CNBuffSize)) goto _output_error;
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : init CCtx for too large level (must fail) : ", testNb++);
            { size_t const r = ZSTD_compressBegin(staticCCtx, ZSTD_maxCLevel());
              if (!ZSTD_isError(r)) goto _output_error; }
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : init CCtx for small level %u (should work again) : ", testNb++, 1);
            CHECK( ZSTD_compressBegin(staticCCtx, 1) );
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : use CStream on CCtx-sized static context (should fail) : ", testNb++);
            CHECK_Z( ZSTD_initCStream(staticCCtx, STATIC_CCTX_LEVEL) ); /* note : doesn't allocate */
            {   ZSTD_outBuffer output = { compressedBuffer, compressedBufferSize, 0 };
                ZSTD_inBuffer input = { CNBuffer, CNBuffSize, 0 };
                size_t const r = ZSTD_compressStream(staticCCtx, &output, &input); /* now allocates, should fail */
                if (!ZSTD_isError(r)) goto _output_error;
            }
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : resize context to CStream size, then stream compress : ", testNb++);
            staticCCtx = ZSTD_initStaticCStream(staticCCtxBuffer, staticCStreamSize);
            assert(staticCCtx != NULL);
            CHECK_Z( ZSTD_initCStream(staticCCtx, STATIC_CCTX_LEVEL) ); /* note : doesn't allocate */
            {   ZSTD_outBuffer output = { compressedBuffer, compressedBufferSize, 0 };
                ZSTD_inBuffer input = { CNBuffer, CNBuffSize, 0 };
                CHECK_Z( ZSTD_compressStream(staticCCtx, &output, &input) );
            }
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : CStream for small level %u : ", testNb++, 1);
            CHECK_Z( ZSTD_initCStream(staticCCtx, 1) ); /* note : doesn't allocate */
            {   ZSTD_outBuffer output = { compressedBuffer, compressedBufferSize, 0 };
                ZSTD_inBuffer input = { CNBuffer, CNBuffSize, 0 };
                CHECK_Z( ZSTD_compressStream(staticCCtx, &output, &input) );
            }
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : init static CStream with dictionary (should fail) : ", testNb++);
            { size_t const r = ZSTD_initCStream_usingDict(staticCCtx, CNBuffer, 64 KB, 1);
              if (!ZSTD_isError(r)) goto _output_error; }
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : use DStream on DCtx-sized static context (should fail) : ", testNb++);
            CHECK_Z( ZSTD_initDStream(staticDCtx) );
            {   ZSTD_outBuffer output = { decodedBuffer, CNBuffSize, 0 };
                ZSTD_inBuffer input = { compressedBuffer, ZSTD_FRAMEHEADERSIZE_MAX+1, 0 };
                size_t const r = ZSTD_decompressStream(staticDCtx, &output, &input);
                if (!ZSTD_isError(r)) goto _output_error;
            }
            DISPLAYLEVEL(3, "OK \n");
        }
        free(staticCCtxBuffer);
        free(staticDCtxBuffer);
    }

    DISPLAYLEVEL(3, "test%3i : Static context sizes for negative levels : ", testNb++);
    {   size_t const cctxSizeN1 = ZSTD_estimateCCtxSize(-1);
        size_t const cctxSizeP1 = ZSTD_estimateCCtxSize(1);
        size_t const cstreamSizeN1 = ZSTD_estimateCStreamSize(-1);
        size_t const cstreamSizeP1 = ZSTD_estimateCStreamSize(1);

        if (!(0 < cctxSizeN1 && cctxSizeN1 <= cctxSizeP1)) goto _output_error;
        if (!(0 < cstreamSizeN1 && cstreamSizeN1 <= cstreamSizeP1)) goto _output_error;
    }
    DISPLAYLEVEL(3, "OK \n");


    /* ZSTDMT simple MT compression test */
    DISPLAYLEVEL(3, "test%3i : create ZSTDMT CCtx : ", testNb++);
    {   ZSTD_CCtx* const mtctx = ZSTD_createCCtx();
        if (mtctx==NULL) {
            DISPLAY("mtctx : not enough memory, aborting \n");
            testResult = 1;
            goto _end;
        }
        CHECK( ZSTD_CCtx_setParameter(mtctx, ZSTD_c_nbWorkers, 2) );
        CHECK( ZSTD_CCtx_setParameter(mtctx, ZSTD_c_compressionLevel, 1) );
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3u : compress %u bytes with 2 threads : ", testNb++, (unsigned)CNBuffSize);
        CHECK_VAR(cSize, ZSTD_compress2(mtctx,
                                compressedBuffer, compressedBufferSize,
                                CNBuffer, CNBuffSize) );
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : decompressed size test : ", testNb++);
        {   unsigned long long const rSize = ZSTD_getFrameContentSize(compressedBuffer, cSize);
            if (rSize != CNBuffSize)  {
                DISPLAY("ZSTD_getFrameContentSize incorrect : %u != %u \n", (unsigned)rSize, (unsigned)CNBuffSize);
                goto _output_error;
        }   }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : decompress %u bytes : ", testNb++, (unsigned)CNBuffSize);
        { size_t const r = ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize);
          if (r != CNBuffSize) goto _output_error; }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : check decompressed result : ", testNb++);
        {   size_t u;
            for (u=0; u<CNBuffSize; u++) {
                if (((BYTE*)decodedBuffer)[u] != ((BYTE*)CNBuffer)[u]) goto _output_error;
        }   }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : compress -T2 with checksum : ", testNb++);
        CHECK( ZSTD_CCtx_setParameter(mtctx, ZSTD_c_checksumFlag, 1) );
        CHECK( ZSTD_CCtx_setParameter(mtctx, ZSTD_c_contentSizeFlag, 1) );
        CHECK( ZSTD_CCtx_setParameter(mtctx, ZSTD_c_overlapLog, 3) );
        CHECK_VAR(cSize, ZSTD_compress2(mtctx,
                                compressedBuffer, compressedBufferSize,
                                CNBuffer, CNBuffSize) );
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : decompress %u bytes : ", testNb++, (unsigned)CNBuffSize);
        { size_t const r = ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize);
          if (r != CNBuffSize) goto _output_error; }
        DISPLAYLEVEL(3, "OK \n");

        ZSTD_freeCCtx(mtctx);
    }

    DISPLAYLEVEL(3, "test%3u : compress empty string and decompress with small window log : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        char out[32];
        if (cctx == NULL || dctx == NULL) goto _output_error;
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0) );
        CHECK_VAR(cSize, ZSTD_compress2(cctx, out, sizeof(out), NULL, 0) );
        DISPLAYLEVEL(3, "OK (%u bytes)\n", (unsigned)cSize);

        CHECK( ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 10) );
        {   char const* outPtr = out;
            ZSTD_inBuffer inBuffer = { outPtr, cSize, 0 };
            ZSTD_outBuffer outBuffer = { NULL, 0, 0 };
            size_t dSize;
            CHECK_VAR(dSize, ZSTD_decompressStream(dctx, &outBuffer, &inBuffer) );
            if (dSize != 0) goto _output_error;
        }

        ZSTD_freeDCtx(dctx);
        ZSTD_freeCCtx(cctx);
    }

    DISPLAYLEVEL(3, "test%3i : compress with block splitting : ", testNb++)
    {   ZSTD_CCtx* cctx = ZSTD_createCCtx();
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_useBlockSplitter, ZSTD_ps_enable) );
        cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
        CHECK(cSize);
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : compress -T2 with/without literals compression : ", testNb++)
    {   ZSTD_CCtx* cctx = ZSTD_createCCtx();
        size_t cSize1, cSize2;
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1) );
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 2) );
        cSize1 = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
        CHECK(cSize1);
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_literalCompressionMode, ZSTD_ps_disable) );
        cSize2 = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
        CHECK(cSize2);
        CHECK_LT(cSize1, cSize2);
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : Multithreaded ZSTD_compress2() with rsyncable : ", testNb++)
    {   ZSTD_CCtx* cctx = ZSTD_createCCtx();
        /* Set rsyncable and don't give the ZSTD_compressBound(CNBuffSize) so
         * ZSTDMT is forced to not take the shortcut.
         */
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1) );
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 1) );
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_rsyncable, 1) );
        CHECK( ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize - 1, CNBuffer, CNBuffSize) );
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : setting multithreaded parameters : ", testNb++)
    {   ZSTD_CCtx_params* params = ZSTD_createCCtxParams();
        int const jobSize = 512 KB;
        int value;
        /* Check that the overlap log and job size are unset. */
        CHECK( ZSTD_CCtxParams_getParameter(params, ZSTD_c_overlapLog, &value) );
        CHECK_EQ(value, 0);
        CHECK( ZSTD_CCtxParams_getParameter(params, ZSTD_c_jobSize, &value) );
        CHECK_EQ(value, 0);
        /* Set and check the overlap log and job size. */
        CHECK( ZSTD_CCtxParams_setParameter(params, ZSTD_c_overlapLog, 5) );
        CHECK( ZSTD_CCtxParams_setParameter(params, ZSTD_c_jobSize, jobSize) );
        CHECK( ZSTD_CCtxParams_getParameter(params, ZSTD_c_overlapLog, &value) );
        CHECK_EQ(value, 5);
        CHECK( ZSTD_CCtxParams_getParameter(params, ZSTD_c_jobSize, &value) );
        CHECK_EQ(value, jobSize);
        /* Set the number of workers and check the overlap log and job size. */
        CHECK( ZSTD_CCtxParams_setParameter(params, ZSTD_c_nbWorkers, 2) );
        CHECK( ZSTD_CCtxParams_getParameter(params, ZSTD_c_overlapLog, &value) );
        CHECK_EQ(value, 5);
        CHECK( ZSTD_CCtxParams_getParameter(params, ZSTD_c_jobSize, &value) );
        CHECK_EQ(value, jobSize);
        ZSTD_freeCCtxParams(params);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Simple API multiframe test */
    DISPLAYLEVEL(3, "test%3i : compress multiple frames : ", testNb++);
    {   size_t off = 0;
        int i;
        int const segs = 4;
        /* only use the first half so we don't push against size limit of compressedBuffer */
        size_t const segSize = (CNBuffSize / 2) / segs;

        const U32 skipLen = 129 KB;
        char* const skipBuff = (char*)malloc(skipLen);
        assert(skipBuff != NULL);
        memset(skipBuff, 0, skipLen);
        for (i = 0; i < segs; i++) {
            CHECK_NEWV(r, ZSTD_compress(
                            (BYTE*)compressedBuffer + off, CNBuffSize - off,
                            (BYTE*)CNBuffer + segSize * (size_t)i, segSize,
                            5) );
            off += r;
            if (i == segs/2) {
                /* insert skippable frame */
                size_t const skippableSize =
                    ZSTD_writeSkippableFrame((BYTE*)compressedBuffer + off, compressedBufferSize,
                                             skipBuff, skipLen, seed % 15);
                CHECK_Z(skippableSize);
                off += skippableSize;
            }
        }
        cSize = off;
        free(skipBuff);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : get decompressed size of multiple frames : ", testNb++);
    {   unsigned long long const r = ZSTD_findDecompressedSize(compressedBuffer, cSize);
        if (r != CNBuffSize / 2) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : get tight decompressed bound of multiple frames : ", testNb++);
    {   unsigned long long const bound = ZSTD_decompressBound(compressedBuffer, cSize);
        if (bound != CNBuffSize / 2) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : decompress multiple frames : ", testNb++);
    {   CHECK_NEWV(r, ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSize));
        if (r != CNBuffSize / 2) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : check decompressed result : ", testNb++);
    if (memcmp(decodedBuffer, CNBuffer, CNBuffSize / 2) != 0) goto _output_error;
    DISPLAYLEVEL(3, "OK \n");

    /* Simple API skippable frame test */
    DISPLAYLEVEL(3, "test%3i : read/write a skippable frame : ", testNb++);
    {   U32 i;
        unsigned readMagic;
        unsigned long long receivedSize;
        size_t skippableSize;
        const U32 skipLen = 129 KB;
        char* const skipBuff = (char*)malloc(skipLen);
        assert(skipBuff != NULL);
        for (i = 0; i < skipLen; i++)
            skipBuff[i] = (char) ((seed + i) % 256);
        skippableSize = ZSTD_writeSkippableFrame(
                                compressedBuffer, compressedBufferSize,
                                skipBuff, skipLen, seed % 15);
        CHECK_Z(skippableSize);
        CHECK_EQ(1, ZSTD_isSkippableFrame(compressedBuffer, skippableSize));
        receivedSize = ZSTD_readSkippableFrame(decodedBuffer, CNBuffSize, &readMagic, compressedBuffer, skippableSize);
        CHECK_EQ(skippableSize, receivedSize + ZSTD_SKIPPABLEHEADERSIZE);
        CHECK_EQ(seed % 15, readMagic);
        if (memcmp(decodedBuffer, skipBuff, skipLen) != 0) goto _output_error;

        free(skipBuff);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : read/write an empty skippable frame : ", testNb++);
    {
        unsigned readMagic;
        unsigned long long receivedSize;
        size_t skippableSize;
        skippableSize = ZSTD_writeSkippableFrame(
                                compressedBuffer, compressedBufferSize,
                                CNBuffer, 0, seed % 15);
        CHECK_EQ(ZSTD_SKIPPABLEHEADERSIZE, skippableSize);
        CHECK_EQ(1, ZSTD_isSkippableFrame(compressedBuffer, skippableSize));
        receivedSize = ZSTD_readSkippableFrame(NULL, 0, &readMagic, compressedBuffer, skippableSize);
        CHECK_EQ(skippableSize, receivedSize + ZSTD_SKIPPABLEHEADERSIZE);
        CHECK_EQ(seed % 15, readMagic);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Dictionary and CCtx Duplication tests */
    {   ZSTD_CCtx* const ctxOrig = ZSTD_createCCtx();
        ZSTD_CCtx* const ctxDuplicated = ZSTD_createCCtx();
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        static const size_t dictSize = 551;
        assert(dctx != NULL); assert(ctxOrig != NULL); assert(ctxDuplicated != NULL);

        DISPLAYLEVEL(3, "test%3i : copy context too soon : ", testNb++);
        { size_t const copyResult = ZSTD_copyCCtx(ctxDuplicated, ctxOrig, 0);
          if (!ZSTD_isError(copyResult)) goto _output_error; }   /* error must be detected */
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : load dictionary into context : ", testNb++);
        CHECK( ZSTD_compressBegin_usingDict(ctxOrig, CNBuffer, dictSize, 2) );
        CHECK( ZSTD_copyCCtx(ctxDuplicated, ctxOrig, 0) ); /* Begin_usingDict implies unknown srcSize, so match that */
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : compress with flat dictionary : ", testNb++);
        cSize = 0;
        CHECKPLUS(r, ZSTD_compressEnd(ctxOrig,
                                      compressedBuffer, compressedBufferSize,
                         (const char*)CNBuffer + dictSize, CNBuffSize - dictSize),
                  cSize += r);
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : frame built with flat dictionary should be decompressible : ", testNb++);
        CHECKPLUS(r, ZSTD_decompress_usingDict(dctx,
                                       decodedBuffer, CNBuffSize,
                                       compressedBuffer, cSize,
                                       CNBuffer, dictSize),
                  if (r != CNBuffSize - dictSize) goto _output_error);
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : compress with duplicated context : ", testNb++);
        {   size_t const cSizeOrig = cSize;
            cSize = 0;
            CHECKPLUS(r, ZSTD_compressEnd(ctxDuplicated,
                                    compressedBuffer, compressedBufferSize,
                       (const char*)CNBuffer + dictSize, CNBuffSize - dictSize),
                      cSize += r);
            if (cSize != cSizeOrig) goto _output_error;   /* should be identical ==> same size */
        }
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : frame built with duplicated context should be decompressible : ", testNb++);
        CHECKPLUS(r, ZSTD_decompress_usingDict(dctx,
                                           decodedBuffer, CNBuffSize,
                                           compressedBuffer, cSize,
                                           CNBuffer, dictSize),
                  if (r != CNBuffSize - dictSize) goto _output_error);
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : decompress with DDict : ", testNb++);
        {   ZSTD_DDict* const ddict = ZSTD_createDDict(CNBuffer, dictSize);
            size_t const r = ZSTD_decompress_usingDDict(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize, ddict);
            if (r != CNBuffSize - dictSize) goto _output_error;
            DISPLAYLEVEL(3, "OK (size of DDict : %u) \n", (unsigned)ZSTD_sizeof_DDict(ddict));
            ZSTD_freeDDict(ddict);
        }

        DISPLAYLEVEL(3, "test%3i : decompress with static DDict : ", testNb++);
        {   size_t const ddictBufferSize = ZSTD_estimateDDictSize(dictSize, ZSTD_dlm_byCopy);
            void* const ddictBuffer = malloc(ddictBufferSize);
            if (ddictBuffer == NULL) goto _output_error;
            {   const ZSTD_DDict* const ddict = ZSTD_initStaticDDict(ddictBuffer, ddictBufferSize, CNBuffer, dictSize, ZSTD_dlm_byCopy, ZSTD_dct_auto);
                size_t const r = ZSTD_decompress_usingDDict(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize, ddict);
                if (r != CNBuffSize - dictSize) goto _output_error;
            }
            free(ddictBuffer);
            DISPLAYLEVEL(3, "OK (size of static DDict : %u) \n", (unsigned)ddictBufferSize);
        }

        DISPLAYLEVEL(3, "test%3i : check content size on duplicated context : ", testNb++);
        {   size_t const testSize = CNBuffSize / 3;
            CHECK( ZSTD_compressBegin(ctxOrig, ZSTD_defaultCLevel()) );
            CHECK( ZSTD_copyCCtx(ctxDuplicated, ctxOrig, testSize) );

            CHECK_VAR(cSize, ZSTD_compressEnd(ctxDuplicated, compressedBuffer, ZSTD_compressBound(testSize),
                                          (const char*)CNBuffer + dictSize, testSize) );
            {   ZSTD_frameHeader zfh;
                if (ZSTD_getFrameHeader(&zfh, compressedBuffer, cSize)) goto _output_error;
                if ((zfh.frameContentSize != testSize) && (zfh.frameContentSize != 0)) goto _output_error;
        }   }
        DISPLAYLEVEL(3, "OK \n");

        /* Note : these tests should be replaced by proper regression tests,
         *         but existing ones do not focus on small data + dictionary + all levels.
         */
        if ((int)(compressibility * 100 + 0.1) == FUZ_compressibility_default) { /* test only valid with known input */
            size_t const flatdictSize = 22 KB;
            size_t const contentSize = 9 KB;
            const void* const dict = (const char*)CNBuffer;
            const void* const contentStart = (const char*)dict + flatdictSize;
            /* These upper bounds are generally within a few bytes of the compressed size */
            size_t target_nodict_cSize[22+1] = { 3840, 3770, 3870, 3830, 3770,
                                                 3770, 3770, 3770, 3750, 3750,
                                                 3742, 3675, 3674, 3665, 3664,
                                                 3663, 3662, 3661, 3660, 3660,
                                                 3660, 3660, 3660 };
            size_t const target_wdict_cSize[22+1] =  { 2830, 2896, 2893, 2820, 2940,
                                                       2950, 2950, 2925, 2900, 2891,
                                                       2910, 2910, 2910, 2780, 2775,
                                                       2765, 2760, 2755, 2754, 2753,
                                                       2753, 2753, 2753 };
            int l = 1;
            int const maxLevel = ZSTD_maxCLevel();
            /* clevels with strategies that support rowhash on small inputs */
            int rowLevel = 4;
            int const rowLevelEnd = 8;

            DISPLAYLEVEL(3, "test%3i : flat-dictionary efficiency test : \n", testNb++);
            assert(maxLevel == 22);
            RDG_genBuffer(CNBuffer, flatdictSize + contentSize, compressibility, 0., seed);
            DISPLAYLEVEL(4, "content hash : %016llx;  dict hash : %016llx \n",
                        (unsigned long long)XXH64(contentStart, contentSize, 0),
                        (unsigned long long)XXH64(dict, flatdictSize, 0));

            for ( ; l <= maxLevel; l++) {
                size_t const nodict_cSize = ZSTD_compress(compressedBuffer, compressedBufferSize,
                                                          contentStart, contentSize, l);
                if (nodict_cSize > target_nodict_cSize[l]) {
                    DISPLAYLEVEL(1, "error : compression at level %i worse than expected (%u > %u) \n",
                                    l, (unsigned)nodict_cSize, (unsigned)target_nodict_cSize[l]);
                    goto _output_error;
                }
                DISPLAYLEVEL(4, "level %i : max expected %u >= reached %u \n",
                                l, (unsigned)target_nodict_cSize[l], (unsigned)nodict_cSize);
            }
            for ( l=1 ; l <= maxLevel; l++) {
                size_t const wdict_cSize = ZSTD_compress_usingDict(ctxOrig,
                                                          compressedBuffer, compressedBufferSize,
                                                          contentStart, contentSize,
                                                          dict, flatdictSize,
                                                          l);
                if (wdict_cSize > target_wdict_cSize[l]) {
                    DISPLAYLEVEL(1, "error : compression with dictionary at level %i worse than expected (%u > %u) \n",
                                    l, (unsigned)wdict_cSize, (unsigned)target_wdict_cSize[l]);
                    goto _output_error;
                }
                DISPLAYLEVEL(4, "level %i with dictionary : max expected %u >= reached %u \n",
                                l, (unsigned)target_wdict_cSize[l], (unsigned)wdict_cSize);
            }
            /* Compression with ZSTD_compress2 and row match finder force enabled.
             * Give some slack for force-enabled row matchfinder since we're on a small input (9KB)
             */
            for ( ; rowLevel <= rowLevelEnd; ++rowLevel) target_nodict_cSize[rowLevel] += 5;
            for (l=1 ; l <= maxLevel; l++) {
                ZSTD_CCtx* const cctx = ZSTD_createCCtx();
                size_t nodict_cSize;
                ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, l);
                ZSTD_CCtx_setParameter(cctx, ZSTD_c_useRowMatchFinder, ZSTD_ps_enable);
                nodict_cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize,
                                                           contentStart, contentSize);
                if (nodict_cSize > target_nodict_cSize[l]) {
                    DISPLAYLEVEL(1, "error : compression with compress2 at level %i worse than expected (%u > %u) \n",
                                    l, (unsigned)nodict_cSize, (unsigned)target_nodict_cSize[l]);
                    ZSTD_freeCCtx(cctx);
                    goto _output_error;
                }
                DISPLAYLEVEL(4, "level %i with compress2 : max expected %u >= reached %u \n",
                                l, (unsigned)target_nodict_cSize[l], (unsigned)nodict_cSize);
                ZSTD_freeCCtx(cctx);
            }
            /* Dict compression with DMS */
            for ( l=1 ; l <= maxLevel; l++) {
                size_t wdict_cSize;
                CHECK_Z( ZSTD_CCtx_loadDictionary(ctxOrig, dict, flatdictSize) );
                CHECK_Z( ZSTD_CCtx_setParameter(ctxOrig, ZSTD_c_compressionLevel, l) );
                CHECK_Z( ZSTD_CCtx_setParameter(ctxOrig, ZSTD_c_enableDedicatedDictSearch, 0) );
                CHECK_Z( ZSTD_CCtx_setParameter(ctxOrig, ZSTD_c_forceAttachDict, ZSTD_dictForceAttach) );
                wdict_cSize = ZSTD_compress2(ctxOrig, compressedBuffer, compressedBufferSize, contentStart, contentSize);
                if (wdict_cSize > target_wdict_cSize[l]) {
                    DISPLAYLEVEL(1, "error : compression with dictionary and compress2 at level %i worse than expected (%u > %u) \n",
                                    l, (unsigned)wdict_cSize, (unsigned)target_wdict_cSize[l]);
                    goto _output_error;
                }
                DISPLAYLEVEL(4, "level %i with dictionary and compress2 : max expected %u >= reached %u \n",
                                l, (unsigned)target_wdict_cSize[l], (unsigned)wdict_cSize);
            }

            DISPLAYLEVEL(4, "compression efficiency tests OK \n");
        }

        ZSTD_freeCCtx(ctxOrig);
        ZSTD_freeCCtx(ctxDuplicated);
        ZSTD_freeDCtx(dctx);
    }

    /* Dictionary and dictBuilder tests */
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        size_t const dictBufferCapacity = 16 KB;
        void* const dictBuffer = malloc(dictBufferCapacity);
        size_t const totalSampleSize = 1 MB;
        size_t const sampleUnitSize = 8 KB;
        U32 const nbSamples = (U32)(totalSampleSize / sampleUnitSize);
        size_t* const samplesSizes = (size_t*) malloc(nbSamples * sizeof(size_t));
        size_t dictSize;
        U32 dictID;
        size_t dictHeaderSize;
        size_t dictBufferFixedSize = 144;
        unsigned char const dictBufferFixed[144] = {0x37, 0xa4, 0x30, 0xec, 0x63, 0x00, 0x00, 0x00, 0x08, 0x10, 0x00, 0x1f,
                                                    0x0f, 0x00, 0x28, 0xe5, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                    0x00, 0x80, 0x0f, 0x9e, 0x0f, 0x00, 0x00, 0x24, 0x40, 0x80, 0x00, 0x01,
                                                    0x02, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0xde, 0x08,
                                                    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
                                                    0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
                                                    0x08, 0x08, 0x08, 0x08, 0xbc, 0xe1, 0x4b, 0x92, 0x0e, 0xb4, 0x7b, 0x18,
                                                    0x86, 0x61, 0x18, 0xc6, 0x18, 0x63, 0x8c, 0x31, 0xc6, 0x18, 0x63, 0x8c,
                                                    0x31, 0x66, 0x66, 0x66, 0x66, 0xb6, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x04,
                                                    0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x20, 0x73, 0x6f, 0x64, 0x61,
                                                    0x6c, 0x65, 0x73, 0x20, 0x74, 0x6f, 0x72, 0x74, 0x6f, 0x72, 0x20, 0x65,
                                                    0x6c, 0x65, 0x69, 0x66, 0x65, 0x6e, 0x64, 0x2e, 0x20, 0x41, 0x6c, 0x69};

        if (dictBuffer==NULL || samplesSizes==NULL) {
            free(dictBuffer);
            free(samplesSizes);
            goto _output_error;
        }

        DISPLAYLEVEL(3, "test%3i : dictBuilder on cyclic data : ", testNb++);
        assert(compressedBufferSize >= totalSampleSize);
        { U32 u; for (u=0; u<totalSampleSize; u++) ((BYTE*)decodedBuffer)[u] = (BYTE)u; }
        { U32 u; for (u=0; u<nbSamples; u++) samplesSizes[u] = sampleUnitSize; }
        {   size_t const sDictSize = ZDICT_trainFromBuffer(dictBuffer, dictBufferCapacity,
                                         decodedBuffer, samplesSizes, nbSamples);
            if (ZDICT_isError(sDictSize)) goto _output_error;
            DISPLAYLEVEL(3, "OK, created dictionary of size %u \n", (unsigned)sDictSize);
        }

        DISPLAYLEVEL(3, "test%3i : dictBuilder : ", testNb++);
        { U32 u; for (u=0; u<nbSamples; u++) samplesSizes[u] = sampleUnitSize; }
        dictSize = ZDICT_trainFromBuffer(dictBuffer, dictBufferCapacity,
                                         CNBuffer, samplesSizes, nbSamples);
        if (ZDICT_isError(dictSize)) goto _output_error;
        DISPLAYLEVEL(3, "OK, created dictionary of size %u \n", (unsigned)dictSize);

        DISPLAYLEVEL(3, "test%3i : Multithreaded COVER dictBuilder : ", testNb++);
        { U32 u; for (u=0; u<nbSamples; u++) samplesSizes[u] = sampleUnitSize; }
        {   ZDICT_cover_params_t coverParams;
            memset(&coverParams, 0, sizeof(coverParams));
            coverParams.steps = 8;
            coverParams.nbThreads = 4;
            dictSize = ZDICT_optimizeTrainFromBuffer_cover(
                dictBuffer, dictBufferCapacity,
                CNBuffer, samplesSizes, nbSamples/8,  /* less samples for faster tests */
                &coverParams);
            if (ZDICT_isError(dictSize)) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK, created dictionary of size %u \n", (unsigned)dictSize);

        DISPLAYLEVEL(3, "test%3i : COVER dictBuilder with shrinkDict: ", testNb++);
        { U32 u; for (u=0; u<nbSamples; u++) samplesSizes[u] = sampleUnitSize; }
        {   ZDICT_cover_params_t coverParams;
            memset(&coverParams, 0, sizeof(coverParams));
            coverParams.steps = 8;
            coverParams.nbThreads = 4;
            coverParams.shrinkDict = 1;
            coverParams.shrinkDictMaxRegression = 1;
            dictSize = ZDICT_optimizeTrainFromBuffer_cover(
                dictBuffer, dictBufferCapacity,
                CNBuffer, samplesSizes, nbSamples/8,  /* less samples for faster tests */
                &coverParams);
            if (ZDICT_isError(dictSize)) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK, created dictionary of size %u \n", (unsigned)dictSize);

        DISPLAYLEVEL(3, "test%3i : Multithreaded FASTCOVER dictBuilder : ", testNb++);
        { U32 u; for (u=0; u<nbSamples; u++) samplesSizes[u] = sampleUnitSize; }
        {   ZDICT_fastCover_params_t fastCoverParams;
            memset(&fastCoverParams, 0, sizeof(fastCoverParams));
            fastCoverParams.steps = 8;
            fastCoverParams.nbThreads = 4;
            dictSize = ZDICT_optimizeTrainFromBuffer_fastCover(
                dictBuffer, dictBufferCapacity,
                CNBuffer, samplesSizes, nbSamples,
                &fastCoverParams);
            if (ZDICT_isError(dictSize)) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK, created dictionary of size %u \n", (unsigned)dictSize);

        DISPLAYLEVEL(3, "test%3i : FASTCOVER dictBuilder with shrinkDict: ", testNb++);
        { U32 u; for (u=0; u<nbSamples; u++) samplesSizes[u] = sampleUnitSize; }
        {   ZDICT_fastCover_params_t fastCoverParams;
            memset(&fastCoverParams, 0, sizeof(fastCoverParams));
            fastCoverParams.steps = 8;
            fastCoverParams.nbThreads = 4;
            fastCoverParams.shrinkDict = 1;
            fastCoverParams.shrinkDictMaxRegression = 1;
            dictSize = ZDICT_optimizeTrainFromBuffer_fastCover(
                dictBuffer, dictBufferCapacity,
                CNBuffer, samplesSizes, nbSamples,
                &fastCoverParams);
            if (ZDICT_isError(dictSize)) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK, created dictionary of size %u \n", (unsigned)dictSize);

        DISPLAYLEVEL(3, "test%3i : check dictID : ", testNb++);
        dictID = ZDICT_getDictID(dictBuffer, dictSize);
        if (dictID==0) goto _output_error;
        DISPLAYLEVEL(3, "OK : %u \n", (unsigned)dictID);

        DISPLAYLEVEL(3, "test%3i : check dict header size no error : ", testNb++);
        dictHeaderSize = ZDICT_getDictHeaderSize(dictBuffer, dictSize);
        if (dictHeaderSize==0) goto _output_error;
        DISPLAYLEVEL(3, "OK : %u \n", (unsigned)dictHeaderSize);

        DISPLAYLEVEL(3, "test%3i : check dict header size correctness : ", testNb++);
        {   dictHeaderSize = ZDICT_getDictHeaderSize(dictBufferFixed, dictBufferFixedSize);
            if (dictHeaderSize != 115) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK : %u \n", (unsigned)dictHeaderSize);

        DISPLAYLEVEL(3, "test%3i : compress with dictionary : ", testNb++);
        cSize = ZSTD_compress_usingDict(cctx, compressedBuffer, compressedBufferSize,
                                        CNBuffer, CNBuffSize,
                                        dictBuffer, dictSize, 4);
        if (ZSTD_isError(cSize)) goto _output_error;
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : retrieve dictID from dictionary : ", testNb++);
        {   U32 const did = ZSTD_getDictID_fromDict(dictBuffer, dictSize);
            if (did != dictID) goto _output_error;   /* non-conformant (content-only) dictionary */
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : retrieve dictID from frame : ", testNb++);
        {   U32 const did = ZSTD_getDictID_fromFrame(compressedBuffer, cSize);
            if (did != dictID) goto _output_error;   /* non-conformant (content-only) dictionary */
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : frame built with dictionary should be decompressible : ", testNb++);
        {   ZSTD_DCtx* const dctx = ZSTD_createDCtx(); assert(dctx != NULL);
            CHECKPLUS(r, ZSTD_decompress_usingDict(dctx,
                                           decodedBuffer, CNBuffSize,
                                           compressedBuffer, cSize,
                                           dictBuffer, dictSize),
                      if (r != CNBuffSize) goto _output_error);
            ZSTD_freeDCtx(dctx);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : estimate CDict size : ", testNb++);
        {   ZSTD_compressionParameters const cParams = ZSTD_getCParams(1, CNBuffSize, dictSize);
            size_t const estimatedSize = ZSTD_estimateCDictSize_advanced(dictSize, cParams, ZSTD_dlm_byRef);
            DISPLAYLEVEL(3, "OK : %u \n", (unsigned)estimatedSize);
        }

        DISPLAYLEVEL(3, "test%3i : compress with CDict ", testNb++);
        {   ZSTD_compressionParameters const cParams = ZSTD_getCParams(1, CNBuffSize, dictSize);
            ZSTD_CDict* const cdict = ZSTD_createCDict_advanced(dictBuffer, dictSize,
                                            ZSTD_dlm_byRef, ZSTD_dct_auto,
                                            cParams, ZSTD_defaultCMem);
            assert(cdict != NULL);
            DISPLAYLEVEL(3, "(size : %u) : ", (unsigned)ZSTD_sizeof_CDict(cdict));
            assert(ZSTD_getDictID_fromDict(dictBuffer, dictSize) == ZSTD_getDictID_fromCDict(cdict));
            cSize = ZSTD_compress_usingCDict(cctx, compressedBuffer, compressedBufferSize,
                                                 CNBuffer, CNBuffSize, cdict);
            ZSTD_freeCDict(cdict);
            if (ZSTD_isError(cSize)) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : retrieve dictID from frame : ", testNb++);
        {   U32 const did = ZSTD_getDictID_fromFrame(compressedBuffer, cSize);
            if (did != dictID) goto _output_error;   /* non-conformant (content-only) dictionary */
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : frame built with dictionary should be decompressible : ", testNb++);
        {   ZSTD_DCtx* const dctx = ZSTD_createDCtx(); assert(dctx != NULL);
            CHECKPLUS(r, ZSTD_decompress_usingDict(dctx,
                                           decodedBuffer, CNBuffSize,
                                           compressedBuffer, cSize,
                                           dictBuffer, dictSize),
                      if (r != CNBuffSize) goto _output_error);
            ZSTD_freeDCtx(dctx);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : compress with static CDict : ", testNb++);
        {   int const maxLevel = ZSTD_maxCLevel();
            int level;
            for (level = 1; level <= maxLevel; ++level) {
                ZSTD_compressionParameters const cParams = ZSTD_getCParams(level, CNBuffSize, dictSize);
                size_t const cdictSize = ZSTD_estimateCDictSize_advanced(dictSize, cParams, ZSTD_dlm_byCopy);
                void* const cdictBuffer = malloc(cdictSize);
                if (cdictBuffer==NULL) goto _output_error;
                {   const ZSTD_CDict* const cdict = ZSTD_initStaticCDict(
                                                cdictBuffer, cdictSize,
                                                dictBuffer, dictSize,
                                                ZSTD_dlm_byCopy, ZSTD_dct_auto,
                                                cParams);
                    if (cdict == NULL) {
                        DISPLAY("ZSTD_initStaticCDict failed ");
                        goto _output_error;
                    }
                    cSize = ZSTD_compress_usingCDict(cctx,
                                    compressedBuffer, compressedBufferSize,
                                    CNBuffer, MIN(10 KB, CNBuffSize), cdict);
                    if (ZSTD_isError(cSize)) {
                        DISPLAY("ZSTD_compress_usingCDict failed ");
                        goto _output_error;
                }   }
                free(cdictBuffer);
        }   }
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : ZSTD_compress_usingCDict_advanced, no contentSize, no dictID : ", testNb++);
        {   ZSTD_frameParameters const fParams = { 0 /* frameSize */, 1 /* checksum */, 1 /* noDictID*/ };
            ZSTD_compressionParameters const cParams = ZSTD_getCParams(1, CNBuffSize, dictSize);
            ZSTD_CDict* const cdict = ZSTD_createCDict_advanced(dictBuffer, dictSize, ZSTD_dlm_byRef, ZSTD_dct_auto, cParams, ZSTD_defaultCMem);
            assert(cdict != NULL);
            cSize = ZSTD_compress_usingCDict_advanced(cctx,
                                                      compressedBuffer, compressedBufferSize,
                                                      CNBuffer, CNBuffSize,
                                                      cdict, fParams);
            ZSTD_freeCDict(cdict);
            if (ZSTD_isError(cSize)) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : try retrieving contentSize from frame : ", testNb++);
        {   U64 const contentSize = ZSTD_getFrameContentSize(compressedBuffer, cSize);
            if (contentSize != ZSTD_CONTENTSIZE_UNKNOWN) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK (unknown)\n");

        DISPLAYLEVEL(3, "test%3i : frame built without dictID should be decompressible : ", testNb++);
        {   ZSTD_DCtx* const dctx = ZSTD_createDCtx();
            assert(dctx != NULL);
            CHECKPLUS(r, ZSTD_decompress_usingDict(dctx,
                                           decodedBuffer, CNBuffSize,
                                           compressedBuffer, cSize,
                                           dictBuffer, dictSize),
                      if (r != CNBuffSize) goto _output_error);
            ZSTD_freeDCtx(dctx);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_compress_advanced, no dictID : ", testNb++);
        {   ZSTD_parameters p = ZSTD_getParams(3, CNBuffSize, dictSize);
            p.fParams.noDictIDFlag = 1;
            cSize = ZSTD_compress_advanced(cctx, compressedBuffer, compressedBufferSize,
                                           CNBuffer, CNBuffSize,
                                           dictBuffer, dictSize, p);
            if (ZSTD_isError(cSize)) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/CNBuffSize*100);

        DISPLAYLEVEL(3, "test%3i : frame built without dictID should be decompressible : ", testNb++);
        {   ZSTD_DCtx* const dctx = ZSTD_createDCtx(); assert(dctx != NULL);
            CHECKPLUS(r, ZSTD_decompress_usingDict(dctx,
                                           decodedBuffer, CNBuffSize,
                                           compressedBuffer, cSize,
                                           dictBuffer, dictSize),
                      if (r != CNBuffSize) goto _output_error);
            ZSTD_freeDCtx(dctx);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : dictionary containing only header should return error : ", testNb++);
        {   ZSTD_DCtx* const dctx = ZSTD_createDCtx();
            assert(dctx != NULL);
            {   const size_t ret = ZSTD_decompress_usingDict(
                    dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize,
                    "\x37\xa4\x30\xec\x11\x22\x33\x44", 8);
                if (ZSTD_getErrorCode(ret) != ZSTD_error_dictionary_corrupted)
                    goto _output_error;
            }
            ZSTD_freeDCtx(dctx);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Building cdict w/ ZSTD_dct_fullDict on a good dictionary : ", testNb++);
        {   ZSTD_compressionParameters const cParams = ZSTD_getCParams(1, CNBuffSize, dictSize);
            ZSTD_CDict* const cdict = ZSTD_createCDict_advanced(dictBuffer, dictSize, ZSTD_dlm_byRef, ZSTD_dct_fullDict, cParams, ZSTD_defaultCMem);
            if (cdict==NULL) goto _output_error;
            ZSTD_freeCDict(cdict);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Building cdict w/ ZSTD_dct_fullDict on a rawContent (must fail) : ", testNb++);
        {   ZSTD_compressionParameters const cParams = ZSTD_getCParams(1, CNBuffSize, dictSize);
            ZSTD_CDict* const cdict = ZSTD_createCDict_advanced((const char*)dictBuffer+1, dictSize-1, ZSTD_dlm_byRef, ZSTD_dct_fullDict, cParams, ZSTD_defaultCMem);
            if (cdict!=NULL) goto _output_error;
            ZSTD_freeCDict(cdict);
        }
        DISPLAYLEVEL(3, "OK \n");

        {   char* rawDictBuffer = (char*)malloc(dictSize);
            assert(rawDictBuffer);
            memcpy(rawDictBuffer, (char*)dictBuffer + 2, dictSize - 2);
            memset(rawDictBuffer + dictSize - 2, 0, 2);
            MEM_writeLE32((char*)rawDictBuffer, ZSTD_MAGIC_DICTIONARY);

            DISPLAYLEVEL(3, "test%3i : Loading rawContent starting with dict header w/ ZSTD_dct_auto should fail : ", testNb++);
            {
                size_t ret;
                /* Either operation is allowed to fail, but one must fail. */
                ret = ZSTD_CCtx_loadDictionary_advanced(
                        cctx, (const char*)rawDictBuffer, dictSize, ZSTD_dlm_byRef, ZSTD_dct_auto);
                if (!ZSTD_isError(ret)) {
                    ret = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100));
                    if (!ZSTD_isError(ret)) goto _output_error;
                }
            }
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : Loading rawContent starting with dict header w/ ZSTD_dct_rawContent should pass : ", testNb++);
            {
                size_t ret;
                ret = ZSTD_CCtx_loadDictionary_advanced(
                        cctx, (const char*)rawDictBuffer, dictSize, ZSTD_dlm_byRef, ZSTD_dct_rawContent);
                if (ZSTD_isError(ret)) goto _output_error;
                ret = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100));
                if (ZSTD_isError(ret)) goto _output_error;
            }
            DISPLAYLEVEL(3, "OK \n");

            DISPLAYLEVEL(3, "test%3i : Testing non-attached CDict with ZSTD_dct_rawContent : ", testNb++);
            {   size_t const srcSize = MIN(CNBuffSize, 100);
                ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
                /* Force the dictionary to be reloaded in raw content mode */
                CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_forceAttachDict, ZSTD_dictForceLoad));
                CHECK_Z(ZSTD_CCtx_loadDictionary_advanced(cctx, rawDictBuffer, dictSize, ZSTD_dlm_byRef, ZSTD_dct_rawContent));
                cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, srcSize);
                CHECK_Z(cSize);
            }
            DISPLAYLEVEL(3, "OK \n");

            free(rawDictBuffer);
        }

        DISPLAYLEVEL(3, "test%3i : ZSTD_CCtx_refCDict() then set parameters : ", testNb++);
        {   ZSTD_CDict* const cdict = ZSTD_createCDict(CNBuffer, dictSize, 1);
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1) );
            CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_hashLog, 12 ));
            CHECK_Z( ZSTD_CCtx_refCDict(cctx, cdict) );
            CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1) );
            CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_hashLog, 12 ));
            ZSTD_freeCDict(cdict);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Loading dictionary before setting parameters is the same as loading after : ", testNb++);
        {
            size_t size1, size2;
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 7) );
            CHECK_Z( ZSTD_CCtx_loadDictionary(cctx, CNBuffer, MIN(CNBuffSize, 10 KB)) );
            size1 = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100 KB));
            if (ZSTD_isError(size1)) goto _output_error;

            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            CHECK_Z( ZSTD_CCtx_loadDictionary(cctx, CNBuffer, MIN(CNBuffSize, 10 KB)) );
            CHECK_Z( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 7) );
            size2 = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100 KB));
            if (ZSTD_isError(size2)) goto _output_error;

            if (size1 != size2) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Loading a dictionary clears the prefix : ", testNb++);
        {
            CHECK_Z( ZSTD_CCtx_refPrefix(cctx, (const char*)dictBuffer, dictSize) );
            CHECK_Z( ZSTD_CCtx_loadDictionary(cctx, (const char*)dictBuffer, dictSize) );
            CHECK_Z( ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100)) );
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Loading a dictionary clears the cdict : ", testNb++);
        {
            ZSTD_CDict* const cdict = ZSTD_createCDict(dictBuffer, dictSize, 1);
            CHECK_Z( ZSTD_CCtx_refCDict(cctx, cdict) );
            CHECK_Z( ZSTD_CCtx_loadDictionary(cctx, (const char*)dictBuffer, dictSize) );
            CHECK_Z( ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100)) );
            ZSTD_freeCDict(cdict);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Loading a cdict clears the prefix : ", testNb++);
        {
            ZSTD_CDict* const cdict = ZSTD_createCDict(dictBuffer, dictSize, 1);
            CHECK_Z( ZSTD_CCtx_refPrefix(cctx, (const char*)dictBuffer, dictSize) );
            CHECK_Z( ZSTD_CCtx_refCDict(cctx, cdict) );
            CHECK_Z( ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100)) );
            ZSTD_freeCDict(cdict);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Loading a cdict clears the dictionary : ", testNb++);
        {
            ZSTD_CDict* const cdict = ZSTD_createCDict(dictBuffer, dictSize, 1);
            CHECK_Z( ZSTD_CCtx_loadDictionary(cctx, (const char*)dictBuffer, dictSize) );
            CHECK_Z( ZSTD_CCtx_refCDict(cctx, cdict) );
            CHECK_Z( ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100)) );
            ZSTD_freeCDict(cdict);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Loading a prefix clears the dictionary : ", testNb++);
        {
            CHECK_Z( ZSTD_CCtx_loadDictionary(cctx, (const char*)dictBuffer, dictSize) );
            CHECK_Z( ZSTD_CCtx_refPrefix(cctx, (const char*)dictBuffer, dictSize) );
            CHECK_Z( ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100)) );
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Loading a prefix clears the cdict : ", testNb++);
        {
            ZSTD_CDict* const cdict = ZSTD_createCDict(dictBuffer, dictSize, 1);
            CHECK_Z( ZSTD_CCtx_refCDict(cctx, cdict) );
            CHECK_Z( ZSTD_CCtx_refPrefix(cctx, (const char*)dictBuffer, dictSize) );
            CHECK_Z( ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100)) );
            ZSTD_freeCDict(cdict);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Loaded dictionary persists across reset session : ", testNb++);
        {
            size_t size1, size2;
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            CHECK_Z( ZSTD_CCtx_loadDictionary(cctx, CNBuffer, MIN(CNBuffSize, 10 KB)) );
            size1 = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100 KB));
            if (ZSTD_isError(size1)) goto _output_error;

            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
            size2 = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100 KB));
            if (ZSTD_isError(size2)) goto _output_error;

            if (size1 != size2) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Loaded dictionary is cleared after resetting parameters : ", testNb++);
        {
            size_t size1, size2;
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            CHECK_Z( ZSTD_CCtx_loadDictionary(cctx, CNBuffer, MIN(CNBuffSize, 10 KB)) );
            size1 = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100 KB));
            if (ZSTD_isError(size1)) goto _output_error;

            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            size2 = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100 KB));
            if (ZSTD_isError(size2)) goto _output_error;

            if (size1 == size2) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK \n");

        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
        CHECK_Z( ZSTD_CCtx_loadDictionary(cctx, dictBuffer, dictSize) );
        cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, MIN(CNBuffSize, 100 KB));
        CHECK_Z(cSize);
        DISPLAYLEVEL(3, "test%3i : ZSTD_decompressDCtx() with dictionary : ", testNb++);
        {
            ZSTD_DCtx* dctx = ZSTD_createDCtx();
            size_t ret;
            /* We should fail to decompress without a dictionary. */
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
            ret = ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize);
            if (!ZSTD_isError(ret)) goto _output_error;
            /* We should succeed to decompress with the dictionary. */
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
            CHECK_Z( ZSTD_DCtx_loadDictionary(dctx, dictBuffer, dictSize) );
            CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize) );
            /* The dictionary should persist across calls. */
            CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize) );
            /* When we reset the context the dictionary is cleared. */
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
            ret = ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize);
            if (!ZSTD_isError(ret)) goto _output_error;
            ZSTD_freeDCtx(dctx);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_decompressDCtx() with ddict : ", testNb++);
        {
            ZSTD_DCtx* dctx = ZSTD_createDCtx();
            ZSTD_DDict* ddict = ZSTD_createDDict(dictBuffer, dictSize);
            size_t ret;
            /* We should succeed to decompress with the ddict. */
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
            CHECK_Z( ZSTD_DCtx_refDDict(dctx, ddict) );
            CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize) );
            /* The ddict should persist across calls. */
            CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize) );
            /* When we reset the context the ddict is cleared. */
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
            ret = ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize);
            if (!ZSTD_isError(ret)) goto _output_error;
            ZSTD_freeDCtx(dctx);
            ZSTD_freeDDict(ddict);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_decompressDCtx() with prefix : ", testNb++);
        {
            ZSTD_DCtx* dctx = ZSTD_createDCtx();
            size_t ret;
            /* We should succeed to decompress with the prefix. */
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
            CHECK_Z( ZSTD_DCtx_refPrefix_advanced(dctx, dictBuffer, dictSize, ZSTD_dct_auto) );
            CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize) );
            /* The prefix should be cleared after the first compression. */
            ret = ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize);
            if (!ZSTD_isError(ret)) goto _output_error;
            ZSTD_freeDCtx(dctx);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Dictionary with non-default repcodes : ", testNb++);
        { U32 u; for (u=0; u<nbSamples; u++) samplesSizes[u] = sampleUnitSize; }
        dictSize = ZDICT_trainFromBuffer(dictBuffer, dictSize,
                                         CNBuffer, samplesSizes, nbSamples);
        if (ZDICT_isError(dictSize)) goto _output_error;
        /* Set all the repcodes to non-default */
        {
            BYTE* dictPtr = (BYTE*)dictBuffer;
            BYTE* dictLimit = dictPtr + dictSize - 12;
            /* Find the repcodes */
            while (dictPtr < dictLimit &&
                   (MEM_readLE32(dictPtr) != 1 || MEM_readLE32(dictPtr + 4) != 4 ||
                    MEM_readLE32(dictPtr + 8) != 8)) {
                ++dictPtr;
            }
            if (dictPtr >= dictLimit) goto _output_error;
            MEM_writeLE32(dictPtr + 0, 10);
            MEM_writeLE32(dictPtr + 4, 10);
            MEM_writeLE32(dictPtr + 8, 10);
            /* Set the last 8 bytes to 'x' */
            memset((BYTE*)dictBuffer + dictSize - 8, 'x', 8);
        }
        /* The optimal parser checks all the repcodes.
         * Make sure at least one is a match >= targetLength so that it is
         * immediately chosen. This will make sure that the compressor and
         * decompressor agree on at least one of the repcodes.
         */
        {   size_t dSize;
            BYTE data[1024];
            ZSTD_DCtx* const dctx = ZSTD_createDCtx();
            ZSTD_compressionParameters const cParams = ZSTD_getCParams(19, CNBuffSize, dictSize);
            ZSTD_CDict* const cdict = ZSTD_createCDict_advanced(dictBuffer, dictSize,
                                            ZSTD_dlm_byRef, ZSTD_dct_auto,
                                            cParams, ZSTD_defaultCMem);
            assert(dctx != NULL); assert(cdict != NULL);
            memset(data, 'x', sizeof(data));
            cSize = ZSTD_compress_usingCDict(cctx, compressedBuffer, compressedBufferSize,
                                             data, sizeof(data), cdict);
            ZSTD_freeCDict(cdict);
            if (ZSTD_isError(cSize)) { DISPLAYLEVEL(5, "Compression error %s : ", ZSTD_getErrorName(cSize)); goto _output_error; }
            dSize = ZSTD_decompress_usingDict(dctx, decodedBuffer, sizeof(data), compressedBuffer, cSize, dictBuffer, dictSize);
            if (ZSTD_isError(dSize)) { DISPLAYLEVEL(5, "Decompression error %s : ", ZSTD_getErrorName(dSize)); goto _output_error; }
            if (memcmp(data, decodedBuffer, sizeof(data))) { DISPLAYLEVEL(5, "Data corruption : "); goto _output_error; }
            ZSTD_freeDCtx(dctx);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : ZSTD_decompressDCtx() with multiple ddicts : ", testNb++);
        {
            const size_t numDicts = 128;
            const size_t numFrames = 4;
            size_t i;
            ZSTD_DCtx* dctx = ZSTD_createDCtx();
            ZSTD_DDict** ddictTable = (ZSTD_DDict**)malloc(sizeof(ZSTD_DDict*)*numDicts);
            ZSTD_CDict** cdictTable = (ZSTD_CDict**)malloc(sizeof(ZSTD_CDict*)*numDicts);
            U32 dictIDSeed = seed;
            /* Create new compressed buffer that will hold frames with differing dictIDs */
            char* dictBufferMulti = (char*)malloc(sizeof(char) * dictBufferFixedSize);  /* Modifiable copy of fixed full dict buffer */

            ZSTD_memcpy(dictBufferMulti, dictBufferFixed, dictBufferFixedSize);
            /* Create a bunch of DDicts with random dict IDs */
            for (i = 0; i < numDicts; ++i) {
                U32 currDictID = FUZ_rand(&dictIDSeed);
                MEM_writeLE32(dictBufferMulti+ZSTD_FRAMEIDSIZE, currDictID);
                ddictTable[i] = ZSTD_createDDict(dictBufferMulti, dictBufferFixedSize);
                cdictTable[i] = ZSTD_createCDict(dictBufferMulti, dictBufferFixedSize, 3);
                if (!ddictTable[i] || !cdictTable[i] || ZSTD_getDictID_fromCDict(cdictTable[i]) != ZSTD_getDictID_fromDDict(ddictTable[i])) {
                    goto _output_error;
                }
            }
            /* Compress a few frames using random CDicts */
            {
                size_t off = 0;
                /* only use the first half so we don't push against size limit of compressedBuffer */
                size_t const segSize = (CNBuffSize / 2) / numFrames;
                for (i = 0; i < numFrames; i++) {
                    size_t dictIdx = FUZ_rand(&dictIDSeed) % numDicts;
                    ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
                    {   CHECK_NEWV(r, ZSTD_compress_usingCDict(cctx,
                                    (BYTE*)compressedBuffer + off, CNBuffSize - off,
                                    (BYTE*)CNBuffer + segSize * (size_t)i, segSize,
                                    cdictTable[dictIdx]));
                        off += r;
                    }
                }
                cSize = off;
            }

            /* We should succeed to decompression even though different dicts were used on different frames */
            ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
            ZSTD_DCtx_setParameter(dctx, ZSTD_d_refMultipleDDicts, ZSTD_rmd_refMultipleDDicts);
            /* Reference every single ddict we made */
            for (i = 0; i < numDicts; ++i) {
                CHECK_Z( ZSTD_DCtx_refDDict(dctx, ddictTable[i]));
            }
            CHECK_Z( ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize) );
            /* Streaming decompression should also work */
            {
                ZSTD_inBuffer in = {compressedBuffer, cSize, 0};
                ZSTD_outBuffer out = {decodedBuffer, CNBuffSize, 0};
                while (in.pos < in.size) {
                    CHECK_Z(ZSTD_decompressStream(dctx, &out, &in));
                }
            }
            ZSTD_freeDCtx(dctx);
            for (i = 0; i < numDicts; ++i) {
                ZSTD_freeCDict(cdictTable[i]);
                ZSTD_freeDDict(ddictTable[i]);
            }
            free(dictBufferMulti);
            free(ddictTable);
            free(cdictTable);
        }
        DISPLAYLEVEL(3, "OK \n");

        ZSTD_freeCCtx(cctx);
        free(dictBuffer);
        free(samplesSizes);
    }

    /* COVER dictionary builder tests */
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        size_t dictSize = 16 KB;
        size_t optDictSize = dictSize;
        void* dictBuffer = malloc(dictSize);
        size_t const totalSampleSize = 1 MB;
        size_t const sampleUnitSize = 8 KB;
        U32 const nbSamples = (U32)(totalSampleSize / sampleUnitSize);
        size_t* const samplesSizes = (size_t*) malloc(nbSamples * sizeof(size_t));
        U32 seed32 = seed;
        ZDICT_cover_params_t params;
        U32 dictID;

        if (dictBuffer==NULL || samplesSizes==NULL) {
            free(dictBuffer);
            free(samplesSizes);
            goto _output_error;
        }

        DISPLAYLEVEL(3, "test%3i : ZDICT_trainFromBuffer_cover : ", testNb++);
        { U32 u; for (u=0; u<nbSamples; u++) samplesSizes[u] = sampleUnitSize; }
        memset(&params, 0, sizeof(params));
        params.d = 1 + (FUZ_rand(&seed32) % 16);
        params.k = params.d + (FUZ_rand(&seed32) % 256);
        dictSize = ZDICT_trainFromBuffer_cover(dictBuffer, dictSize,
                                               CNBuffer, samplesSizes, nbSamples,
                                               params);
        if (ZDICT_isError(dictSize)) goto _output_error;
        DISPLAYLEVEL(3, "OK, created dictionary of size %u \n", (unsigned)dictSize);

        DISPLAYLEVEL(3, "test%3i : check dictID : ", testNb++);
        dictID = ZDICT_getDictID(dictBuffer, dictSize);
        if (dictID==0) goto _output_error;
        DISPLAYLEVEL(3, "OK : %u \n", (unsigned)dictID);

        DISPLAYLEVEL(3, "test%3i : ZDICT_optimizeTrainFromBuffer_cover : ", testNb++);
        memset(&params, 0, sizeof(params));
        params.steps = 4;
        optDictSize = ZDICT_optimizeTrainFromBuffer_cover(dictBuffer, optDictSize,
                                                          CNBuffer, samplesSizes,
                                                          nbSamples / 4, &params);
        if (ZDICT_isError(optDictSize)) goto _output_error;
        DISPLAYLEVEL(3, "OK, created dictionary of size %u \n", (unsigned)optDictSize);

        DISPLAYLEVEL(3, "test%3i : check dictID : ", testNb++);
        dictID = ZDICT_getDictID(dictBuffer, optDictSize);
        if (dictID==0) goto _output_error;
        DISPLAYLEVEL(3, "OK : %u \n", (unsigned)dictID);

        ZSTD_freeCCtx(cctx);
        free(dictBuffer);
        free(samplesSizes);
    }

    /* Decompression defense tests */
    DISPLAYLEVEL(3, "test%3i : Check input length for magic number : ", testNb++);
    { size_t const r = ZSTD_decompress(decodedBuffer, CNBuffSize, CNBuffer, 3);   /* too small input */
      if (!ZSTD_isError(r)) goto _output_error;
      if (ZSTD_getErrorCode(r) != ZSTD_error_srcSize_wrong) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : Check magic Number : ", testNb++);
    ((char*)(CNBuffer))[0] = 1;
    { size_t const r = ZSTD_decompress(decodedBuffer, CNBuffSize, CNBuffer, 4);
      if (!ZSTD_isError(r)) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    /* content size verification test */
    DISPLAYLEVEL(3, "test%3i : Content size verification : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        size_t const srcSize = 5000;
        size_t const wrongSrcSize = (srcSize + 1000);
        ZSTD_parameters params = ZSTD_getParams(1, wrongSrcSize, 0);
        params.fParams.contentSizeFlag = 1;
        CHECK( ZSTD_compressBegin_advanced(cctx, NULL, 0, params, wrongSrcSize) );
        {   size_t const result = ZSTD_compressEnd(cctx, decodedBuffer, CNBuffSize, CNBuffer, srcSize);
            if (!ZSTD_isError(result)) goto _output_error;
            if (ZSTD_getErrorCode(result) != ZSTD_error_srcSize_wrong) goto _output_error;
            DISPLAYLEVEL(3, "OK : %s \n", ZSTD_getErrorName(result));
        }
        ZSTD_freeCCtx(cctx);
    }

    /* negative compression level test : ensure simple API and advanced API produce same result */
    DISPLAYLEVEL(3, "test%3i : negative compression level : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        size_t const srcSize = CNBuffSize / 5;
        int const compressionLevel = -1;

        assert(cctx != NULL);
        {   size_t const cSize_1pass = ZSTD_compress(compressedBuffer, compressedBufferSize,
                                                     CNBuffer, srcSize, compressionLevel);
            if (ZSTD_isError(cSize_1pass)) goto _output_error;

            CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, compressionLevel) );
            {   size_t const compressionResult = ZSTD_compress2(cctx,
                                    compressedBuffer, compressedBufferSize,
                                    CNBuffer, srcSize);
                DISPLAYLEVEL(5, "simple=%zu vs %zu=advanced : ", cSize_1pass, compressionResult);
                if (ZSTD_isError(compressionResult)) goto _output_error;
                if (compressionResult != cSize_1pass) goto _output_error;
        }   }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* parameters order test */
    {   size_t const inputSize = CNBuffSize / 2;
        U64 xxh64;

        {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
            DISPLAYLEVEL(3, "test%3i : parameters in order : ", testNb++);
            assert(cctx != NULL);
            CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 2) );
            CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1) );
            CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 18) );
            {   size_t const compressedSize = ZSTD_compress2(cctx,
                                compressedBuffer, ZSTD_compressBound(inputSize),
                                CNBuffer, inputSize);
                CHECK(compressedSize);
                cSize = compressedSize;
                xxh64 = XXH64(compressedBuffer, compressedSize, 0);
            }
            DISPLAYLEVEL(3, "OK (compress : %u -> %u bytes)\n", (unsigned)inputSize, (unsigned)cSize);
            ZSTD_freeCCtx(cctx);
        }

        {   ZSTD_CCtx* cctx = ZSTD_createCCtx();
            DISPLAYLEVEL(3, "test%3i : parameters disordered : ", testNb++);
            CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 18) );
            CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1) );
            CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 2) );
            {   size_t const result = ZSTD_compress2(cctx,
                                compressedBuffer, ZSTD_compressBound(inputSize),
                                CNBuffer, inputSize);
                CHECK(result);
                if (result != cSize) goto _output_error;   /* must result in same compressed result, hence same size */
                if (XXH64(compressedBuffer, result, 0) != xxh64) goto _output_error;  /* must result in exactly same content, hence same hash */
                DISPLAYLEVEL(3, "OK (compress : %u -> %u bytes)\n", (unsigned)inputSize, (unsigned)result);
            }
            ZSTD_freeCCtx(cctx);
        }
    }

    /* advanced parameters for decompression */
    {   ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        assert(dctx != NULL);

        DISPLAYLEVEL(3, "test%3i : get dParameter bounds ", testNb++);
        {   ZSTD_bounds const bounds = ZSTD_dParam_getBounds(ZSTD_d_windowLogMax);
            CHECK(bounds.error);
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : wrong dParameter : ", testNb++);
        {   size_t const sr = ZSTD_DCtx_setParameter(dctx, (ZSTD_dParameter)999999, 0);
            if (!ZSTD_isError(sr)) goto _output_error;
        }
        {   ZSTD_bounds const bounds = ZSTD_dParam_getBounds((ZSTD_dParameter)999998);
            if (!ZSTD_isError(bounds.error)) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : out of bound dParameter : ", testNb++);
        {   size_t const sr = ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 9999);
            if (!ZSTD_isError(sr)) goto _output_error;
        }
        {   size_t const sr = ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, (ZSTD_format_e)888);
            if (!ZSTD_isError(sr)) goto _output_error;
        }
        DISPLAYLEVEL(3, "OK \n");

        ZSTD_freeDCtx(dctx);
    }


    /* custom formats tests */
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        size_t const inputSize = CNBuffSize / 2;   /* won't cause pb with small dict size */
        assert(dctx != NULL); assert(cctx != NULL);

        /* basic block compression */
        DISPLAYLEVEL(3, "test%3i : magic-less format test : ", testNb++);
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_format, ZSTD_f_zstd1_magicless) );
        {   ZSTD_inBuffer in = { CNBuffer, inputSize, 0 };
            ZSTD_outBuffer out = { compressedBuffer, ZSTD_compressBound(inputSize), 0 };
            size_t const result = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
            if (result != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
            cSize = out.pos;
        }
        DISPLAYLEVEL(3, "OK (compress : %u -> %u bytes)\n", (unsigned)inputSize, (unsigned)cSize);

        DISPLAYLEVEL(3, "test%3i : decompress normally (should fail) : ", testNb++);
        {   size_t const decodeResult = ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize);
            if (ZSTD_getErrorCode(decodeResult) != ZSTD_error_prefix_unknown) goto _output_error;
            DISPLAYLEVEL(3, "OK : %s \n", ZSTD_getErrorName(decodeResult));
        }

        DISPLAYLEVEL(3, "test%3i : decompress of magic-less frame : ", testNb++);
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
        CHECK( ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless) );
        {   ZSTD_frameHeader zfh;
            size_t const zfhrt = ZSTD_getFrameHeader_advanced(&zfh, compressedBuffer, cSize, ZSTD_f_zstd1_magicless);
            if (zfhrt != 0) goto _output_error;
        }
        /* one shot */
        {   size_t const result = ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize);
            if (result != inputSize) goto _output_error;
            DISPLAYLEVEL(3, "one-shot OK, ");
        }
        /* streaming */
        {   ZSTD_inBuffer in = { compressedBuffer, cSize, 0 };
            ZSTD_outBuffer out = { decodedBuffer, CNBuffSize, 0 };
            size_t const result = ZSTD_decompressStream(dctx, &out, &in);
            if (result != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
            if (out.pos != inputSize) goto _output_error;
            DISPLAYLEVEL(3, "streaming OK : regenerated %u bytes \n", (unsigned)out.pos);
        }

        /* basic block compression */
        DISPLAYLEVEL(3, "test%3i : empty magic-less format test : ", testNb++);
        CHECK( ZSTD_CCtx_setParameter(cctx, ZSTD_c_format, ZSTD_f_zstd1_magicless) );
        {   ZSTD_inBuffer in = { CNBuffer, 0, 0 };
            ZSTD_outBuffer out = { compressedBuffer, ZSTD_compressBound(0), 0 };
            size_t const result = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
            if (result != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
            cSize = out.pos;
        }
        DISPLAYLEVEL(3, "OK (compress : %u -> %u bytes)\n", (unsigned)0, (unsigned)cSize);

        DISPLAYLEVEL(3, "test%3i : decompress of empty magic-less frame : ", testNb++);
        ZSTD_DCtx_reset(dctx, ZSTD_reset_session_and_parameters);
        CHECK( ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless) );
        /* one shot */
        {   size_t const result = ZSTD_decompressDCtx(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize);
            if (result != 0) goto _output_error;
            DISPLAYLEVEL(3, "one-shot OK, ");
        }
        /* streaming */
        {   ZSTD_inBuffer in = { compressedBuffer, cSize, 0 };
            ZSTD_outBuffer out = { decodedBuffer, CNBuffSize, 0 };
            size_t const result = ZSTD_decompressStream(dctx, &out, &in);
            if (result != 0) goto _output_error;
            if (in.pos != in.size) goto _output_error;
            if (out.pos != 0) goto _output_error;
            DISPLAYLEVEL(3, "streaming OK : regenerated %u bytes \n", (unsigned)out.pos);
        }

        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
    }

    DISPLAYLEVEL(3, "test%3i : Decompression parameter reset test : ", testNb++);
    {
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        /* Attempt to future proof this to new parameters. */
        int const maxParam = 2000;
        int param;
        if (ZSTD_d_experimentalParam3 > maxParam) goto _output_error;
        for (param = 0; param < maxParam; ++param) {
            ZSTD_dParameter dParam = (ZSTD_dParameter)param;
            ZSTD_bounds bounds = ZSTD_dParam_getBounds(dParam);
            int value1;
            int value2;
            int check;
            if (ZSTD_isError(bounds.error))
                continue;
            CHECK(ZSTD_DCtx_getParameter(dctx, dParam, &value1));
            value2 = (value1 != bounds.lowerBound) ? bounds.lowerBound : bounds.upperBound;
            CHECK(ZSTD_DCtx_setParameter(dctx, dParam, value2));
            CHECK(ZSTD_DCtx_getParameter(dctx, dParam, &check));
            if (check != value2) goto _output_error;
            CHECK(ZSTD_DCtx_reset(dctx, ZSTD_reset_parameters));
            CHECK(ZSTD_DCtx_getParameter(dctx, dParam, &check));
            if (check != value1) goto _output_error;
        }
        ZSTD_freeDCtx(dctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* block API tests */
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        static const size_t dictSize = 65 KB;
        static const size_t blockSize = 100 KB;   /* won't cause pb with small dict size */
        size_t cSize2;
        assert(cctx != NULL); assert(dctx != NULL);

        /* basic block compression */
        DISPLAYLEVEL(3, "test%3i : Block compression test : ", testNb++);
        CHECK( ZSTD_compressBegin(cctx, 5) );
        CHECK( ZSTD_getBlockSize(cctx) >= blockSize);
        CHECK_VAR(cSize, ZSTD_compressBlock(cctx, compressedBuffer, ZSTD_compressBound(blockSize), CNBuffer, blockSize) );
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Block decompression test : ", testNb++);
        CHECK( ZSTD_decompressBegin(dctx) );
        { CHECK_NEWV(r, ZSTD_decompressBlock(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize) );
          if (r != blockSize) goto _output_error; }
        DISPLAYLEVEL(3, "OK \n");

        /* very long stream of block compression */
        DISPLAYLEVEL(3, "test%3i : Huge block streaming compression test : ", testNb++);
        CHECK( ZSTD_compressBegin(cctx, -199) );  /* we just want to quickly overflow internal U32 index */
        CHECK( ZSTD_getBlockSize(cctx) >= blockSize);
        {   U64 const toCompress = 5000000000ULL;   /* > 4 GB */
            U64 compressed = 0;
            while (compressed < toCompress) {
                size_t const blockCSize = ZSTD_compressBlock(cctx, compressedBuffer, ZSTD_compressBound(blockSize), CNBuffer, blockSize);
                assert(blockCSize != 0);
                if (ZSTD_isError(blockCSize)) goto _output_error;
                compressed += blockCSize;
        }   }
        DISPLAYLEVEL(3, "OK \n");

        /* dictionary block compression */
        DISPLAYLEVEL(3, "test%3i : Dictionary Block compression test : ", testNb++);
        CHECK( ZSTD_compressBegin_usingDict(cctx, CNBuffer, dictSize, 5) );
        CHECK_VAR(cSize,  ZSTD_compressBlock(cctx, compressedBuffer, ZSTD_compressBound(blockSize), (char*)CNBuffer+dictSize, blockSize));
        RDG_genBuffer((char*)CNBuffer+dictSize+blockSize, blockSize, 0.0, 0.0, seed);  /* create a non-compressible second block */
        { CHECK_NEWV(r, ZSTD_compressBlock(cctx, (char*)compressedBuffer+cSize, ZSTD_compressBound(blockSize), (char*)CNBuffer+dictSize+blockSize, blockSize) );  /* for cctx history consistency */
          assert(r == 0); /* non-compressible block */ }
        memcpy((char*)compressedBuffer+cSize, (char*)CNBuffer+dictSize+blockSize, blockSize);   /* send non-compressed block (without header) */
        CHECK_VAR(cSize2, ZSTD_compressBlock(cctx, (char*)compressedBuffer+cSize+blockSize, ZSTD_compressBound(blockSize),
                                                   (char*)CNBuffer+dictSize+2*blockSize, blockSize));
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Dictionary Block decompression test : ", testNb++);
        CHECK( ZSTD_decompressBegin_usingDict(dctx, CNBuffer, dictSize) );
        {   CHECK_NEWV( r, ZSTD_decompressBlock(dctx, decodedBuffer, blockSize, compressedBuffer, cSize) );
            if (r != blockSize) {
                DISPLAYLEVEL(1, "ZSTD_decompressBlock() with _usingDict() fails : %u, instead of %u expected \n", (unsigned)r, (unsigned)blockSize);
                goto _output_error;
        }   }
        memcpy((char*)decodedBuffer+blockSize, (char*)compressedBuffer+cSize, blockSize);
        ZSTD_insertBlock(dctx, (char*)decodedBuffer+blockSize, blockSize);   /* insert non-compressed block into dctx history */
        {   CHECK_NEWV( r, ZSTD_decompressBlock(dctx, (char*)decodedBuffer+2*blockSize, blockSize, (char*)compressedBuffer+cSize+blockSize, cSize2) );
            if (r != blockSize) {
                DISPLAYLEVEL(1, "ZSTD_decompressBlock() with _usingDict() and after insertBlock() fails : %u, instead of %u expected \n", (unsigned)r, (unsigned)blockSize);
                goto _output_error;
        }   }
        assert(memcpy((char*)CNBuffer+dictSize, decodedBuffer, blockSize*3));  /* ensure regenerated content is identical to origin */
        DISPLAYLEVEL(3, "OK \n");

        DISPLAYLEVEL(3, "test%3i : Block compression with CDict : ", testNb++);
        {   ZSTD_CDict* const cdict = ZSTD_createCDict(CNBuffer, dictSize, 3);
            if (cdict==NULL) goto _output_error;
            CHECK( ZSTD_compressBegin_usingCDict(cctx, cdict) );
            CHECK( ZSTD_compressBlock(cctx, compressedBuffer, ZSTD_compressBound(blockSize), (char*)CNBuffer+dictSize, blockSize) );
            ZSTD_freeCDict(cdict);
        }
        DISPLAYLEVEL(3, "OK \n");

        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
    }

    /* long rle test */
    {   size_t sampleSize = 0;
        size_t expectedCompressedSize = 39; /* block 1, 2: compressed, block 3: RLE, zstd 1.4.4 */
        DISPLAYLEVEL(3, "test%3i : Long RLE test : ", testNb++);
        memset((char*)CNBuffer+sampleSize, 'B', 256 KB - 1);
        sampleSize += 256 KB - 1;
        memset((char*)CNBuffer+sampleSize, 'A', 96 KB);
        sampleSize += 96 KB;
        cSize = ZSTD_compress(compressedBuffer, ZSTD_compressBound(sampleSize), CNBuffer, sampleSize, 1);
        if (ZSTD_isError(cSize) || cSize > expectedCompressedSize) goto _output_error;
        { CHECK_NEWV(regenSize, ZSTD_decompress(decodedBuffer, sampleSize, compressedBuffer, cSize));
          if (regenSize!=sampleSize) goto _output_error; }
        DISPLAYLEVEL(3, "OK \n");
    }

    DISPLAYLEVEL(3, "test%3i : ZSTD_generateSequences decode from sequences test : ", testNb++);
    {
        size_t srcSize = 150 KB;
        BYTE* src = (BYTE*)CNBuffer;
        BYTE* decoded = (BYTE*)compressedBuffer;

        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        ZSTD_Sequence* seqs = (ZSTD_Sequence*)malloc(srcSize * sizeof(ZSTD_Sequence));
        size_t seqsSize;

        if (seqs == NULL) goto _output_error;
        assert(cctx != NULL);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 19);
        /* Populate src with random data */
        RDG_genBuffer(CNBuffer, srcSize, compressibility, 0.5, seed);

        /* Test with block delimiters roundtrip */
        seqsSize = ZSTD_generateSequences(cctx, seqs, srcSize, src, srcSize);
        FUZ_decodeSequences(decoded, seqs, seqsSize, src, srcSize, ZSTD_sf_explicitBlockDelimiters);
        assert(!memcmp(CNBuffer, compressedBuffer, srcSize));

        /* Test no block delimiters roundtrip */
        seqsSize = ZSTD_mergeBlockDelimiters(seqs, seqsSize);
        FUZ_decodeSequences(decoded, seqs, seqsSize, src, srcSize, ZSTD_sf_noBlockDelimiters);
        assert(!memcmp(CNBuffer, compressedBuffer, srcSize));

        ZSTD_freeCCtx(cctx);
        free(seqs);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_getSequences followed by ZSTD_compressSequences : ", testNb++);
    {
        size_t srcSize = 500 KB;
        BYTE* src = (BYTE*)CNBuffer;
        BYTE* dst = (BYTE*)compressedBuffer;
        size_t dstSize = ZSTD_compressBound(srcSize);
        size_t decompressSize = srcSize;
        char* decompressBuffer = (char*)malloc(decompressSize);
        size_t compressedSize;
        size_t dSize;

        ZSTD_CCtx* cctx = ZSTD_createCCtx();
        ZSTD_Sequence* seqs = (ZSTD_Sequence*)malloc(srcSize * sizeof(ZSTD_Sequence));
        size_t seqsSize;

        if (seqs == NULL) goto _output_error;
        assert(cctx != NULL);

        /* Populate src with random data */
        RDG_genBuffer(CNBuffer, srcSize, compressibility, 0., seed);

        /* Test with block delimiters roundtrip */
        seqsSize = ZSTD_generateSequences(cctx, seqs, srcSize, src, srcSize);
        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_explicitBlockDelimiters);
        compressedSize = ZSTD_compressSequences(cctx, dst, dstSize, seqs, seqsSize, src, srcSize);
        if (ZSTD_isError(compressedSize)) {
            DISPLAY("Error in sequence compression with block delims\n");
            goto _output_error;
        }
        dSize = ZSTD_decompress(decompressBuffer, decompressSize, dst, compressedSize);
        if (ZSTD_isError(dSize)) {
            DISPLAY("Error in sequence compression roundtrip with block delims\n");
            goto _output_error;
        }
        assert(!memcmp(decompressBuffer, src, srcSize));

        /* Test with no block delimiters roundtrip */
        seqsSize = ZSTD_mergeBlockDelimiters(seqs, seqsSize);
        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
        ZSTD_CCtx_setParameter(cctx, ZSTD_c_blockDelimiters, ZSTD_sf_noBlockDelimiters);
        compressedSize = ZSTD_compressSequences(cctx, dst, dstSize, seqs, seqsSize, src, srcSize);
        if (ZSTD_isError(compressedSize)) {
            DISPLAY("Error in sequence compression with no block delims\n");
            goto _output_error;
        }
        dSize = ZSTD_decompress(decompressBuffer, decompressSize, dst, compressedSize);
        if (ZSTD_isError(dSize)) {
            DISPLAY("Error in sequence compression roundtrip with no block delims\n");
            goto _output_error;
        }
        assert(!memcmp(decompressBuffer, src, srcSize));

        ZSTD_freeCCtx(cctx);
        free(decompressBuffer);
        free(seqs);
    }
    DISPLAYLEVEL(3, "OK \n");

    /* Multiple blocks of zeros test */
    #define LONGZEROSLENGTH 1000000 /* 1MB of zeros */
    DISPLAYLEVEL(3, "test%3i : compress %u zeroes : ", testNb++, LONGZEROSLENGTH);
    memset(CNBuffer, 0, LONGZEROSLENGTH);
    CHECK_VAR(cSize, ZSTD_compress(compressedBuffer, ZSTD_compressBound(LONGZEROSLENGTH), CNBuffer, LONGZEROSLENGTH, 1) );
    DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/LONGZEROSLENGTH*100);

    DISPLAYLEVEL(3, "test%3i : decompress %u zeroes : ", testNb++, LONGZEROSLENGTH);
    { CHECK_NEWV(r, ZSTD_decompress(decodedBuffer, LONGZEROSLENGTH, compressedBuffer, cSize) );
      if (r != LONGZEROSLENGTH) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    /* All zeroes test (test bug #137) */
    #define ZEROESLENGTH 100
    DISPLAYLEVEL(3, "test%3i : compress %u zeroes : ", testNb++, ZEROESLENGTH);
    memset(CNBuffer, 0, ZEROESLENGTH);
    CHECK_VAR(cSize, ZSTD_compress(compressedBuffer, ZSTD_compressBound(ZEROESLENGTH), CNBuffer, ZEROESLENGTH, 1) );
    DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/ZEROESLENGTH*100);

    DISPLAYLEVEL(3, "test%3i : decompress %u zeroes : ", testNb++, ZEROESLENGTH);
    { CHECK_NEWV(r, ZSTD_decompress(decodedBuffer, ZEROESLENGTH, compressedBuffer, cSize) );
      if (r != ZEROESLENGTH) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    /* nbSeq limit test */
    #define _3BYTESTESTLENGTH 131000
    #define NB3BYTESSEQLOG   9
    #define NB3BYTESSEQ     (1 << NB3BYTESSEQLOG)
    #define NB3BYTESSEQMASK (NB3BYTESSEQ-1)
    /* creates a buffer full of 3-bytes sequences */
    {   BYTE _3BytesSeqs[NB3BYTESSEQ][3];
        U32 rSeed = 1;

        /* create batch of 3-bytes sequences */
        {   int i;
            for (i=0; i < NB3BYTESSEQ; i++) {
                _3BytesSeqs[i][0] = (BYTE)(FUZ_rand(&rSeed) & 255);
                _3BytesSeqs[i][1] = (BYTE)(FUZ_rand(&rSeed) & 255);
                _3BytesSeqs[i][2] = (BYTE)(FUZ_rand(&rSeed) & 255);
        }   }

        /* randomly fills CNBuffer with prepared 3-bytes sequences */
        {   int i;
            for (i=0; i < _3BYTESTESTLENGTH; i += 3) {   /* note : CNBuffer size > _3BYTESTESTLENGTH+3 */
                U32 const id = FUZ_rand(&rSeed) & NB3BYTESSEQMASK;
                ((BYTE*)CNBuffer)[i+0] = _3BytesSeqs[id][0];
                ((BYTE*)CNBuffer)[i+1] = _3BytesSeqs[id][1];
                ((BYTE*)CNBuffer)[i+2] = _3BytesSeqs[id][2];
    }   }   }
    DISPLAYLEVEL(3, "test%3i : growing nbSeq : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        size_t const maxNbSeq = _3BYTESTESTLENGTH / 3;
        size_t const bound = ZSTD_compressBound(_3BYTESTESTLENGTH);
        size_t nbSeq = 1;
        while (nbSeq <= maxNbSeq) {
          CHECK(ZSTD_compressCCtx(cctx, compressedBuffer, bound, CNBuffer, nbSeq * 3, 19));
          /* Check every sequence for the first 100, then skip more rapidly. */
          if (nbSeq < 100) {
            ++nbSeq;
          } else {
            nbSeq += (nbSeq >> 2);
          }
        }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : compress lots 3-bytes sequences : ", testNb++);
    CHECK_VAR(cSize, ZSTD_compress(compressedBuffer, ZSTD_compressBound(_3BYTESTESTLENGTH),
                                   CNBuffer, _3BYTESTESTLENGTH, 19) );
    DISPLAYLEVEL(3, "OK (%u bytes : %.2f%%)\n", (unsigned)cSize, (double)cSize/_3BYTESTESTLENGTH*100);

    DISPLAYLEVEL(3, "test%3i : decompress lots 3-bytes sequence : ", testNb++);
    { CHECK_NEWV(r, ZSTD_decompress(decodedBuffer, _3BYTESTESTLENGTH, compressedBuffer, cSize) );
      if (r != _3BYTESTESTLENGTH) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");


    DISPLAYLEVEL(3, "test%3i : growing literals buffer : ", testNb++);
    RDG_genBuffer(CNBuffer, CNBuffSize, 0.0, 0.1, seed);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        size_t const bound = ZSTD_compressBound(CNBuffSize);
        size_t size = 1;
        while (size <= CNBuffSize) {
          CHECK(ZSTD_compressCCtx(cctx, compressedBuffer, bound, CNBuffer, size, 3));
          /* Check every size for the first 100, then skip more rapidly. */
          if (size < 100) {
            ++size;
          } else {
            size += (size >> 2);
          }
        }
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : incompressible data and ill suited dictionary : ", testNb++);
    {   /* Train a dictionary on low characters */
        size_t dictSize = 16 KB;
        void* const dictBuffer = malloc(dictSize);
        size_t const totalSampleSize = 1 MB;
        size_t const sampleUnitSize = 8 KB;
        U32 const nbSamples = (U32)(totalSampleSize / sampleUnitSize);
        size_t* const samplesSizes = (size_t*) malloc(nbSamples * sizeof(size_t));
        if (!dictBuffer || !samplesSizes) goto _output_error;
        { U32 u; for (u=0; u<nbSamples; u++) samplesSizes[u] = sampleUnitSize; }
        dictSize = ZDICT_trainFromBuffer(dictBuffer, dictSize, CNBuffer, samplesSizes, nbSamples);
        if (ZDICT_isError(dictSize)) goto _output_error;
        /* Reverse the characters to make the dictionary ill suited */
        {   U32 u;
            for (u = 0; u < CNBuffSize; ++u) {
              ((BYTE*)CNBuffer)[u] = 255 - ((BYTE*)CNBuffer)[u];
            }
        }
        {   /* Compress the data */
            size_t const inputSize = 500;
            size_t const outputSize = ZSTD_compressBound(inputSize);
            void* const outputBuffer = malloc(outputSize);
            ZSTD_CCtx* const cctx = ZSTD_createCCtx();
            if (!outputBuffer || !cctx) goto _output_error;
            CHECK(ZSTD_compress_usingDict(cctx, outputBuffer, outputSize, CNBuffer, inputSize, dictBuffer, dictSize, 1));
            free(outputBuffer);
            ZSTD_freeCCtx(cctx);
        }

        free(dictBuffer);
        free(samplesSizes);
    }
    DISPLAYLEVEL(3, "OK \n");


    /* findFrameCompressedSize on skippable frames */
    DISPLAYLEVEL(3, "test%3i : frame compressed size of skippable frame : ", testNb++);
    {   const char* frame = "\x50\x2a\x4d\x18\x05\x0\x0\0abcde";
        size_t const frameSrcSize = 13;
        if (ZSTD_findFrameCompressedSize(frame, frameSrcSize) != frameSrcSize) goto _output_error; }
    DISPLAYLEVEL(3, "OK \n");

    /* error string tests */
    DISPLAYLEVEL(3, "test%3i : testing ZSTD error code strings : ", testNb++);
    if (strcmp("No error detected", ZSTD_getErrorName((ZSTD_ErrorCode)(0-ZSTD_error_no_error))) != 0) goto _output_error;
    if (strcmp("No error detected", ZSTD_getErrorString(ZSTD_error_no_error)) != 0) goto _output_error;
    if (strcmp("Unspecified error code", ZSTD_getErrorString((ZSTD_ErrorCode)(0-ZSTD_error_GENERIC))) != 0) goto _output_error;
    if (strcmp("Error (generic)", ZSTD_getErrorName((size_t)0-ZSTD_error_GENERIC)) != 0) goto _output_error;
    if (strcmp("Error (generic)", ZSTD_getErrorString(ZSTD_error_GENERIC)) != 0) goto _output_error;
    if (strcmp("No error detected", ZSTD_getErrorName(ZSTD_error_GENERIC)) != 0) goto _output_error;
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : testing ZSTD dictionary sizes : ", testNb++);
    RDG_genBuffer(CNBuffer, CNBuffSize, compressibility, 0., seed);
    {
        size_t const size = MIN(128 KB, CNBuffSize);
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_CDict* const lgCDict = ZSTD_createCDict(CNBuffer, size, 1);
        ZSTD_CDict* const smCDict = ZSTD_createCDict(CNBuffer, 1 KB, 1);
        ZSTD_frameHeader lgHeader;
        ZSTD_frameHeader smHeader;

        CHECK_Z(ZSTD_compress_usingCDict(cctx, compressedBuffer, compressedBufferSize, CNBuffer, size, lgCDict));
        CHECK_Z(ZSTD_getFrameHeader(&lgHeader, compressedBuffer, compressedBufferSize));
        CHECK_Z(ZSTD_compress_usingCDict(cctx, compressedBuffer, compressedBufferSize, CNBuffer, size, smCDict));
        CHECK_Z(ZSTD_getFrameHeader(&smHeader, compressedBuffer, compressedBufferSize));

        if (lgHeader.windowSize != smHeader.windowSize) goto _output_error;

        ZSTD_freeCDict(smCDict);
        ZSTD_freeCDict(lgCDict);
        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : testing FSE_normalizeCount() PR#1255: ", testNb++);
    {
        short norm[32];
        unsigned count[32];
        unsigned const tableLog = 5;
        size_t const nbSeq = 32;
        unsigned const maxSymbolValue = 31;
        size_t i;

        for (i = 0; i < 32; ++i)
            count[i] = 1;
        /* Calling FSE_normalizeCount() on a uniform distribution should not
         * cause a division by zero.
         */
        FSE_normalizeCount(norm, tableLog, count, nbSeq, maxSymbolValue, /* useLowProbCount */ 1);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : testing FSE_writeNCount() PR#2779: ", testNb++);
    {
        size_t const outBufSize = 9;
        short const count[11] = {1, 0, 1, 0, 1, 0, 1, 0, 1, 9, 18};
        unsigned const tableLog = 5;
        unsigned const maxSymbolValue = 10;
        BYTE* outBuf = (BYTE*)malloc(outBufSize*sizeof(BYTE));

        /* Ensure that this write doesn't write out of bounds, and that
         * FSE_writeNCount_generic() is *not* called with writeIsSafe == 1.
         */
        FSE_writeNCount(outBuf, outBufSize, count, maxSymbolValue, tableLog);
        free(outBuf);
    }
    DISPLAYLEVEL(3, "OK \n");

#ifdef ZSTD_MULTITHREAD
    DISPLAYLEVEL(3, "test%3i : passing wrong full dict should fail on compressStream2 refPrefix ", testNb++);
    {   ZSTD_CCtx* cctx = ZSTD_createCCtx();
        size_t const srcSize = 1 MB + 5;   /* A little more than ZSTDMT_JOBSIZE_MIN */
        size_t const dstSize = ZSTD_compressBound(srcSize);
        void* const src = CNBuffer;
        void* const dst = compressedBuffer;
        void* dict = (void*)malloc(srcSize);

        RDG_genBuffer(src, srcSize, compressibility, 0.5, seed);
        RDG_genBuffer(dict, srcSize, compressibility, 0., seed);

        /* Make sure there is no ZSTD_MAGIC_NUMBER */
        memset(dict, 0, sizeof(U32));

        /* something more than 1 */
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 2));
        /* lie and claim this is a full dict */
        CHECK_Z(ZSTD_CCtx_refPrefix_advanced(cctx, dict, srcSize, ZSTD_dct_fullDict));

        {   ZSTD_outBuffer out = {dst, dstSize, 0};
            ZSTD_inBuffer in = {src, srcSize, 0};
            /* should fail because its not a full dict like we said it was */
            assert(ZSTD_isError(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush)));
        }

        ZSTD_freeCCtx(cctx);
        free(dict);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : small dictionary with multithreading and LDM ", testNb++);
    {   ZSTD_CCtx* cctx = ZSTD_createCCtx();
        size_t const srcSize = 1 MB + 5;   /* A little more than ZSTDMT_JOBSIZE_MIN */
        size_t const dictSize = 10;
        size_t const dstSize = ZSTD_compressBound(srcSize);
        void* const src = CNBuffer;
        void* const dst = compressedBuffer;
        void* dict = (void*)malloc(dictSize);

        RDG_genBuffer(src, srcSize, compressibility, 0.5, seed);
        RDG_genBuffer(dict, dictSize, compressibility, 0., seed);

        /* Make sure there is no ZSTD_MAGIC_NUMBER */
        memset(dict, 0, sizeof(U32));

        /* Enable MT, LDM, and use refPrefix() for a small dict */
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 2));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1));
        CHECK_Z(ZSTD_CCtx_refPrefix(cctx, dict, dictSize));

        CHECK_Z(ZSTD_compress2(cctx, dst, dstSize, src, srcSize));

        ZSTD_freeCCtx(cctx);
        free(dict);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_getCParams() + dictionary ", testNb++);
    {
        ZSTD_compressionParameters const medium = ZSTD_getCParams(1, 16*1024-1, 0);
        ZSTD_compressionParameters const large = ZSTD_getCParams(1, 128*1024-1, 0);
        ZSTD_compressionParameters const smallDict = ZSTD_getCParams(1, 0, 400);
        ZSTD_compressionParameters const mediumDict = ZSTD_getCParams(1, 0, 10000);
        ZSTD_compressionParameters const largeDict = ZSTD_getCParams(1, 0, 100000);

        assert(!memcmp(&smallDict, &mediumDict, sizeof(smallDict)));
        assert(!memcmp(&medium, &mediumDict, sizeof(medium)));
        assert(!memcmp(&large, &largeDict, sizeof(large)));
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : ZSTD_adjustCParams() + dictionary ", testNb++);
    {
        ZSTD_compressionParameters const cParams = ZSTD_getCParams(1, 0, 0);
        ZSTD_compressionParameters const smallDict = ZSTD_adjustCParams(cParams, 0, 400);
        ZSTD_compressionParameters const smallSrcAndDict = ZSTD_adjustCParams(cParams, 500, 400);

        assert(smallSrcAndDict.windowLog == 10);
        assert(!memcmp(&cParams, &smallDict, sizeof(cParams)));
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : check compression mem usage monotonicity over levels for estimateCCtxSize() : ", testNb++);
    {
        int level = 1;
        size_t prevSize = 0;
        for (; level < ZSTD_maxCLevel(); ++level) {
            size_t const currSize = ZSTD_estimateCCtxSize(level);
            if (prevSize > currSize) {
                DISPLAYLEVEL(3, "Error! previous cctx size: %zu at level: %d is larger than current cctx size: %zu at level: %d",
                             prevSize, level-1, currSize, level);
                goto _output_error;
            }
            prevSize = currSize;
        }
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : check estimateCCtxSize() always larger or equal to ZSTD_estimateCCtxSize_usingCParams() : ", testNb++);
    {
        size_t const kSizeIncrement = 2 KB;
        int level = -3;

        for (; level <= ZSTD_maxCLevel(); ++level) {
            size_t dictSize = 0;
            for (; dictSize <= 256 KB; dictSize += 8 * kSizeIncrement) {
                size_t srcSize = 2 KB;
                for (; srcSize < 300 KB; srcSize += kSizeIncrement) {
                    ZSTD_compressionParameters const cParams = ZSTD_getCParams(level, srcSize, dictSize);
                    size_t const cctxSizeUsingCParams = ZSTD_estimateCCtxSize_usingCParams(cParams);
                    size_t const cctxSizeUsingLevel = ZSTD_estimateCCtxSize(level);
                    if (cctxSizeUsingLevel < cctxSizeUsingCParams
                     || ZSTD_isError(cctxSizeUsingCParams)
                     || ZSTD_isError(cctxSizeUsingLevel)) {
                        DISPLAYLEVEL(3, "error! l: %d dict: %zu srcSize: %zu cctx size cpar: %zu, cctx size level: %zu\n",
                                     level, dictSize, srcSize, cctxSizeUsingCParams, cctxSizeUsingLevel);
                        goto _output_error;
    }   }   }   }   }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "test%3i : thread pool API tests : \n", testNb++)
    {
        int const threadPoolTestResult = threadPoolTests();
        if (threadPoolTestResult) {
            goto _output_error;
        }
    }
    DISPLAYLEVEL(3, "thread pool tests OK \n");

#endif /* ZSTD_MULTITHREAD */

_end:
    free(CNBuffer);
    free(compressedBuffer);
    free(decodedBuffer);
    return testResult;

_output_error:
    testResult = 1;
    DISPLAY("Error detected in Unit tests ! \n");
    goto _end;
}

static int longUnitTests(U32 const seed, double compressibility)
{
    size_t const CNBuffSize = 5 MB;
    void* const CNBuffer = malloc(CNBuffSize);
    size_t const compressedBufferSize = ZSTD_compressBound(CNBuffSize);
    void* const compressedBuffer = malloc(compressedBufferSize);
    void* const decodedBuffer = malloc(CNBuffSize);
    int testResult = 0;
    unsigned testNb=0;
    size_t cSize;

    /* Create compressible noise */
    if (!CNBuffer || !compressedBuffer || !decodedBuffer) {
        DISPLAY("Not enough memory, aborting\n");
        testResult = 1;
        goto _end;
    }
    RDG_genBuffer(CNBuffer, CNBuffSize, compressibility, 0., seed);

    /* note : this test is rather long, it would be great to find a way to speed up its execution */
    DISPLAYLEVEL(3, "longtest%3i : table cleanliness through index reduction : ", testNb++);
    {   int cLevel;
        size_t approxIndex = 0;
        size_t maxIndex = ((3U << 29) + (1U << ZSTD_WINDOWLOG_MAX)); /* ZSTD_CURRENT_MAX from zstd_compress_internal.h */

        /* Provision enough space in a static context so that we can do all
         * this without ever reallocating, which would reset the indices. */
        size_t const staticCCtxSize = ZSTD_estimateCStreamSize(22);
        void* const staticCCtxBuffer = malloc(staticCCtxSize);
        ZSTD_CCtx* const cctx = ZSTD_initStaticCCtx(staticCCtxBuffer, staticCCtxSize);

        /* bump the indices so the following compressions happen at high
         * indices. */
        {   ZSTD_outBuffer out = { compressedBuffer, compressedBufferSize, 0 };
            ZSTD_inBuffer in = { CNBuffer, CNBuffSize, 0 };
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, -500));
            while (approxIndex <= (maxIndex / 4) * 3) {
                CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush));
                approxIndex += in.pos;
                CHECK(in.pos == in.size);
                in.pos = 0;
                out.pos = 0;
            }
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));
        }

        /* spew a bunch of stuff into the table area */
        for (cLevel = 1; cLevel <= 22; cLevel++) {
            ZSTD_outBuffer out = { compressedBuffer, compressedBufferSize / (unsigned)cLevel, 0 };
            ZSTD_inBuffer in = { CNBuffer, CNBuffSize, 0 };
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, cLevel));
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush));
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));
            approxIndex += in.pos;
        }

        /* now crank the indices so we overflow */
        {   ZSTD_outBuffer out = { compressedBuffer, compressedBufferSize, 0 };
            ZSTD_inBuffer in = { CNBuffer, CNBuffSize, 0 };
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, -500));
            while (approxIndex <= maxIndex) {
                CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush));
                approxIndex += in.pos;
                CHECK(in.pos == in.size);
                in.pos = 0;
                out.pos = 0;
            }
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));
        }

        /* do a bunch of compressions again in low indices and ensure we don't
         * hit untracked invalid indices */
        for (cLevel = 1; cLevel <= 22; cLevel++) {
            ZSTD_outBuffer out = { compressedBuffer, compressedBufferSize / (unsigned)cLevel, 0 };
            ZSTD_inBuffer in = { CNBuffer, CNBuffSize, 0 };
            ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
            CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, cLevel));
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush));
            CHECK_Z(ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end));
            approxIndex += in.pos;
        }

        free(staticCCtxBuffer);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "longtest%3i : testing ldm no regressions in size for opt parser : ", testNb++);
    {   size_t cSizeLdm;
        size_t cSizeNoLdm;
        ZSTD_CCtx* const cctx = ZSTD_createCCtx();

        RDG_genBuffer(CNBuffer, CNBuffSize, 0.5, 0.5, seed);

        /* Enable checksum to verify round trip. */
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 19));

        /* Round trip once with ldm. */
        cSizeLdm = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
        CHECK_Z(cSizeLdm);
        CHECK_Z(ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSizeLdm));

        ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters);
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_enableLongDistanceMatching, 0));
        CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 19));

        /* Round trip once without ldm. */
        cSizeNoLdm = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
        CHECK_Z(cSizeNoLdm);
        CHECK_Z(ZSTD_decompress(decodedBuffer, CNBuffSize, compressedBuffer, cSizeNoLdm));

        if (cSizeLdm > cSizeNoLdm) {
            DISPLAY("Using long mode should not cause regressions for btopt+\n");
            testResult = 1;
            goto _end;
        }

        ZSTD_freeCCtx(cctx);
    }
    DISPLAYLEVEL(3, "OK \n");

    DISPLAYLEVEL(3, "longtest%3i : testing cdict compression with different attachment strategies : ", testNb++);
    {   ZSTD_CCtx* const cctx = ZSTD_createCCtx();
        ZSTD_DCtx* const dctx = ZSTD_createDCtx();
        size_t dictSize = CNBuffSize;
        void* dict = (void*)malloc(dictSize);
        ZSTD_CCtx_params* cctx_params = ZSTD_createCCtxParams();
        ZSTD_dictAttachPref_e const attachPrefs[] = {
            ZSTD_dictDefaultAttach,
            ZSTD_dictForceAttach,
            ZSTD_dictForceCopy,
            ZSTD_dictForceLoad,
            ZSTD_dictDefaultAttach,
            ZSTD_dictForceAttach,
            ZSTD_dictForceCopy,
            ZSTD_dictForceLoad
        };
        int const enableDedicatedDictSearch[] = {0, 0, 0, 0, 1, 1, 1, 1};
        int cLevel;
        int i;

        RDG_genBuffer(dict, dictSize, 0.5, 0.5, seed);
        RDG_genBuffer(CNBuffer, CNBuffSize, 0.6, 0.6, seed);

        CHECK(cctx_params != NULL);

        for (dictSize = CNBuffSize; dictSize; dictSize = dictSize >> 3) {
            DISPLAYLEVEL(3, "\n    Testing with dictSize %u ", (U32)dictSize);
            for (cLevel = 4; cLevel < 13; cLevel++) {
                for (i = 0; i < 8; ++i) {
                    ZSTD_dictAttachPref_e const attachPref = attachPrefs[i];
                    int const enableDDS = enableDedicatedDictSearch[i];
                    ZSTD_CDict* cdict;

                    DISPLAYLEVEL(5, "\n      dictSize %u cLevel %d iter %d ", (U32)dictSize, cLevel, i);

                    ZSTD_CCtxParams_init(cctx_params, cLevel);
                    CHECK_Z(ZSTD_CCtxParams_setParameter(cctx_params, ZSTD_c_enableDedicatedDictSearch, enableDDS));

                    cdict = ZSTD_createCDict_advanced2(dict, dictSize, ZSTD_dlm_byRef, ZSTD_dct_auto, cctx_params, ZSTD_defaultCMem);
                    CHECK(cdict != NULL);

                    CHECK_Z(ZSTD_CCtx_refCDict(cctx, cdict));
                    CHECK_Z(ZSTD_CCtx_setParameter(cctx, ZSTD_c_forceAttachDict, (int)attachPref));

                    cSize = ZSTD_compress2(cctx, compressedBuffer, compressedBufferSize, CNBuffer, CNBuffSize);
                    CHECK_Z(cSize);
                    CHECK_Z(ZSTD_decompress_usingDict(dctx, decodedBuffer, CNBuffSize, compressedBuffer, cSize, dict, dictSize));

                    DISPLAYLEVEL(5, "compressed to %u bytes ", (U32)cSize);

                    CHECK_Z(ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters));
                    ZSTD_freeCDict(cdict);
        }   }   }

        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
        ZSTD_freeCCtxParams(cctx_params);
        free(dict);
    }
    DISPLAYLEVEL(3, "OK \n");

_end:
    free(CNBuffer);
    free(compressedBuffer);
    free(decodedBuffer);
    return testResult;

_output_error:
    testResult = 1;
    DISPLAY("Error detected in Unit tests ! \n");
    goto _end;
}


static size_t findDiff(const void* buf1, const void* buf2, size_t max)
{
    const BYTE* b1 = (const BYTE*)buf1;
    const BYTE* b2 = (const BYTE*)buf2;
    size_t u;
    for (u=0; u<max; u++) {
        if (b1[u] != b2[u]) break;
    }
    return u;
}


static ZSTD_parameters FUZ_makeParams(ZSTD_compressionParameters cParams, ZSTD_frameParameters fParams)
{
    ZSTD_parameters params;
    params.cParams = cParams;
    params.fParams = fParams;
    return params;
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

#undef CHECK
#define CHECK(cond, ...) {                                    \
    if (cond) {                                               \
        DISPLAY("Error => ");                                 \
        DISPLAY(__VA_ARGS__);                                 \
        DISPLAY(" (seed %u, test nb %u)  \n", (unsigned)seed, testNb);  \
        goto _output_error;                                   \
}   }

#undef CHECK_Z
#define CHECK_Z(f) {                                          \
    size_t const err = f;                                     \
    if (ZSTD_isError(err)) {                                  \
        DISPLAY("Error => %s : %s ",                          \
                #f, ZSTD_getErrorName(err));                  \
        DISPLAY(" (seed %u, test nb %u)  \n", (unsigned)seed, testNb);  \
        goto _output_error;                                   \
}   }


static int fuzzerTests(U32 seed, unsigned nbTests, unsigned startTest, U32 const maxDurationS, double compressibility, int bigTests)
{
    static const U32 maxSrcLog = 23;
    static const U32 maxSampleLog = 22;
    size_t const srcBufferSize = (size_t)1<<maxSrcLog;
    size_t const dstBufferSize = (size_t)1<<maxSampleLog;
    size_t const cBufferSize   = ZSTD_compressBound(dstBufferSize);
    BYTE* cNoiseBuffer[5];
    BYTE* const cBuffer = (BYTE*) malloc (cBufferSize);
    BYTE* const dstBuffer = (BYTE*) malloc (dstBufferSize);
    BYTE* const mirrorBuffer = (BYTE*) malloc (dstBufferSize);
    ZSTD_CCtx* const refCtx = ZSTD_createCCtx();
    ZSTD_CCtx* const ctx = ZSTD_createCCtx();
    ZSTD_DCtx* const dctx = ZSTD_createDCtx();
    U32 result = 0;
    unsigned testNb = 0;
    U32 coreSeed = seed;
    UTIL_time_t const startClock = UTIL_getTime();
    U64 const maxClockSpan = maxDurationS * SEC_TO_MICRO;
    int const cLevelLimiter = bigTests ? 3 : 2;

    /* allocation */
    cNoiseBuffer[0] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[1] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[2] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[3] = (BYTE*)malloc (srcBufferSize);
    cNoiseBuffer[4] = (BYTE*)malloc (srcBufferSize);
    CHECK (!cNoiseBuffer[0] || !cNoiseBuffer[1] || !cNoiseBuffer[2] || !cNoiseBuffer[3] || !cNoiseBuffer[4]
           || !dstBuffer || !mirrorBuffer || !cBuffer || !refCtx || !ctx || !dctx,
           "Not enough memory, fuzzer tests cancelled");

    /* Create initial samples */
    RDG_genBuffer(cNoiseBuffer[0], srcBufferSize, 0.00, 0., coreSeed);    /* pure noise */
    RDG_genBuffer(cNoiseBuffer[1], srcBufferSize, 0.05, 0., coreSeed);    /* barely compressible */
    RDG_genBuffer(cNoiseBuffer[2], srcBufferSize, compressibility, 0., coreSeed);
    RDG_genBuffer(cNoiseBuffer[3], srcBufferSize, 0.95, 0., coreSeed);    /* highly compressible */
    RDG_genBuffer(cNoiseBuffer[4], srcBufferSize, 1.00, 0., coreSeed);    /* sparse content */

    /* catch up testNb */
    for (testNb=1; testNb < startTest; testNb++) FUZ_rand(&coreSeed);

    /* main test loop */
    for ( ; (testNb <= nbTests) || (UTIL_clockSpanMicro(startClock) < maxClockSpan); testNb++ ) {
        BYTE* srcBuffer;   /* jumping pointer */
        U32 lseed;
        size_t sampleSize, maxTestSize, totalTestSize;
        size_t cSize, totalCSize, totalGenSize;
        U64 crcOrig;
        BYTE* sampleBuffer;
        const BYTE* dict;
        size_t dictSize;

        /* notification */
        if (nbTests >= testNb) { DISPLAYUPDATE(2, "\r%6u/%6u    ", testNb, nbTests); }
        else { DISPLAYUPDATE(2, "\r%6u          ", testNb); }

        FUZ_rand(&coreSeed);
        { U32 const prime1 = 2654435761U; lseed = coreSeed ^ prime1; }

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

        /* select src segment */
        sampleSize = FUZ_randomLength(&lseed, maxSampleLog);

        /* create sample buffer (to catch read error with valgrind & sanitizers)  */
        sampleBuffer = (BYTE*)malloc(sampleSize);
        CHECK(sampleBuffer==NULL, "not enough memory for sample buffer");
        { size_t const sampleStart = FUZ_rand(&lseed) % (srcBufferSize - sampleSize);
          memcpy(sampleBuffer, srcBuffer + sampleStart, sampleSize); }
        crcOrig = XXH64(sampleBuffer, sampleSize, 0);

        /* compression tests */
        {   int const cLevelPositive = (int)
                    ( FUZ_rand(&lseed) %
                     ((U32)ZSTD_maxCLevel() - (FUZ_highbit32((U32)sampleSize) / (U32)cLevelLimiter)) )
                    + 1;
            int const cLevel = ((FUZ_rand(&lseed) & 15) == 3) ?
                             - (int)((FUZ_rand(&lseed) & 7) + 1) :   /* test negative cLevel */
                             cLevelPositive;
            DISPLAYLEVEL(5, "fuzzer t%u: Simple compression test (level %i) \n", testNb, cLevel);
            cSize = ZSTD_compressCCtx(ctx, cBuffer, cBufferSize, sampleBuffer, sampleSize, cLevel);
            CHECK(ZSTD_isError(cSize), "ZSTD_compressCCtx failed : %s", ZSTD_getErrorName(cSize));

            /* compression failure test : too small dest buffer */
            assert(cSize > 3);
            {   const size_t missing = (FUZ_rand(&lseed) % (cSize-2)) + 1;
                const size_t tooSmallSize = cSize - missing;
                const unsigned endMark = 0x4DC2B1A9;
                memcpy(dstBuffer+tooSmallSize, &endMark, sizeof(endMark));
                DISPLAYLEVEL(5, "fuzzer t%u: compress into too small buffer of size %u (missing %u bytes) \n",
                            testNb, (unsigned)tooSmallSize, (unsigned)missing);
                { size_t const errorCode = ZSTD_compressCCtx(ctx, dstBuffer, tooSmallSize, sampleBuffer, sampleSize, cLevel);
                  CHECK(ZSTD_getErrorCode(errorCode) != ZSTD_error_dstSize_tooSmall, "ZSTD_compressCCtx should have failed ! (buffer too small : %u < %u)", (unsigned)tooSmallSize, (unsigned)cSize); }
                { unsigned endCheck; memcpy(&endCheck, dstBuffer+tooSmallSize, sizeof(endCheck));
                  CHECK(endCheck != endMark, "ZSTD_compressCCtx : dst buffer overflow  (check.%08X != %08X.mark)", endCheck, endMark); }
        }   }

        /* frame header decompression test */
        {   ZSTD_frameHeader zfh;
            CHECK_Z( ZSTD_getFrameHeader(&zfh, cBuffer, cSize) );
            CHECK(zfh.frameContentSize != sampleSize, "Frame content size incorrect");
        }

        /* Decompressed size test */
        {   unsigned long long const rSize = ZSTD_findDecompressedSize(cBuffer, cSize);
            CHECK(rSize != sampleSize, "decompressed size incorrect");
        }

        /* successful decompression test */
        DISPLAYLEVEL(5, "fuzzer t%u: simple decompression test \n", testNb);
        {   size_t const margin = (FUZ_rand(&lseed) & 1) ? 0 : (FUZ_rand(&lseed) & 31) + 1;
            size_t const dSize = ZSTD_decompress(dstBuffer, sampleSize + margin, cBuffer, cSize);
            CHECK(dSize != sampleSize, "ZSTD_decompress failed (%s) (srcSize : %u ; cSize : %u)", ZSTD_getErrorName(dSize), (unsigned)sampleSize, (unsigned)cSize);
            {   U64 const crcDest = XXH64(dstBuffer, sampleSize, 0);
                CHECK(crcOrig != crcDest, "decompression result corrupted (pos %u / %u)", (unsigned)findDiff(sampleBuffer, dstBuffer, sampleSize), (unsigned)sampleSize);
        }   }

        free(sampleBuffer);   /* no longer useful after this point */

        /* truncated src decompression test */
        DISPLAYLEVEL(5, "fuzzer t%u: decompression of truncated source \n", testNb);
        {   size_t const missing = (FUZ_rand(&lseed) % (cSize-2)) + 1;   /* no problem, as cSize > 4 (frameHeaderSizer) */
            size_t const tooSmallSize = cSize - missing;
            void* cBufferTooSmall = malloc(tooSmallSize);   /* valgrind will catch read overflows */
            CHECK(cBufferTooSmall == NULL, "not enough memory !");
            memcpy(cBufferTooSmall, cBuffer, tooSmallSize);
            { size_t const errorCode = ZSTD_decompress(dstBuffer, dstBufferSize, cBufferTooSmall, tooSmallSize);
              CHECK(!ZSTD_isError(errorCode), "ZSTD_decompress should have failed ! (truncated src buffer)"); }
            free(cBufferTooSmall);
        }

        /* too small dst decompression test */
        DISPLAYLEVEL(5, "fuzzer t%u: decompress into too small dst buffer \n", testNb);
        if (sampleSize > 3) {
            size_t const missing = (FUZ_rand(&lseed) % (sampleSize-2)) + 1;   /* no problem, as cSize > 4 (frameHeaderSizer) */
            size_t const tooSmallSize = sampleSize - missing;
            static const BYTE token = 0xA9;
            dstBuffer[tooSmallSize] = token;
            { size_t const errorCode = ZSTD_decompress(dstBuffer, tooSmallSize, cBuffer, cSize);
              CHECK(ZSTD_getErrorCode(errorCode) != ZSTD_error_dstSize_tooSmall, "ZSTD_decompress should have failed : %u > %u (dst buffer too small)", (unsigned)errorCode, (unsigned)tooSmallSize); }
            CHECK(dstBuffer[tooSmallSize] != token, "ZSTD_decompress : dst buffer overflow");
        }

        /* noisy src decompression test */
        if (cSize > 6) {
            /* insert noise into src */
            {   U32 const maxNbBits = FUZ_highbit32((U32)(cSize-4));
                size_t pos = 4;   /* preserve magic number (too easy to detect) */
                for (;;) {
                    /* keep some original src */
                    {   U32 const nbBits = FUZ_rand(&lseed) % maxNbBits;
                        size_t const mask = (1<<nbBits) - 1;
                        size_t const skipLength = FUZ_rand(&lseed) & mask;
                        pos += skipLength;
                    }
                    if (pos >= cSize) break;
                    /* add noise */
                    {   U32 const nbBitsCodes = FUZ_rand(&lseed) % maxNbBits;
                        U32 const nbBits = nbBitsCodes ? nbBitsCodes-1 : 0;
                        size_t const mask = (1<<nbBits) - 1;
                        size_t const rNoiseLength = (FUZ_rand(&lseed) & mask) + 1;
                        size_t const noiseLength = MIN(rNoiseLength, cSize-pos);
                        size_t const noiseStart = FUZ_rand(&lseed) % (srcBufferSize - noiseLength);
                        memcpy(cBuffer + pos, srcBuffer + noiseStart, noiseLength);
                        pos += noiseLength;
            }   }   }

            /* decompress noisy source */
            DISPLAYLEVEL(5, "fuzzer t%u: decompress noisy source \n", testNb);
            {   U32 const endMark = 0xA9B1C3D6;
                memcpy(dstBuffer+sampleSize, &endMark, 4);
                {   size_t const decompressResult = ZSTD_decompress(dstBuffer, sampleSize, cBuffer, cSize);
                    /* result *may* be an unlikely success, but even then, it must strictly respect dst buffer boundaries */
                    CHECK((!ZSTD_isError(decompressResult)) && (decompressResult>sampleSize),
                          "ZSTD_decompress on noisy src : result is too large : %u > %u (dst buffer)", (unsigned)decompressResult, (unsigned)sampleSize);
                }
                {   U32 endCheck; memcpy(&endCheck, dstBuffer+sampleSize, 4);
                    CHECK(endMark!=endCheck, "ZSTD_decompress on noisy src : dst buffer overflow");
        }   }   }   /* noisy src decompression test */

        /*=====   Bufferless streaming compression test, scattered segments and dictionary   =====*/
        DISPLAYLEVEL(5, "fuzzer t%u: Bufferless streaming compression test \n", testNb);
        {   U32 const testLog = FUZ_rand(&lseed) % maxSrcLog;
            U32 const dictLog = FUZ_rand(&lseed) % maxSrcLog;
            int const cLevel = (FUZ_rand(&lseed) %
                                (ZSTD_maxCLevel() -
                                 (MAX(testLog, dictLog) / cLevelLimiter))) +
                               1;
            maxTestSize = FUZ_rLogLength(&lseed, testLog);
            if (maxTestSize >= dstBufferSize) maxTestSize = dstBufferSize-1;

            dictSize = FUZ_rLogLength(&lseed, dictLog);   /* needed also for decompression */
            dict = srcBuffer + (FUZ_rand(&lseed) % (srcBufferSize - dictSize));

            DISPLAYLEVEL(6, "fuzzer t%u: Compressing up to <=%u bytes at level %i with dictionary size %u \n",
                            testNb, (unsigned)maxTestSize, cLevel, (unsigned)dictSize);

            if (FUZ_rand(&lseed) & 0xF) {
                CHECK_Z ( ZSTD_compressBegin_usingDict(refCtx, dict, dictSize, cLevel) );
            } else {
                ZSTD_compressionParameters const cPar = ZSTD_getCParams(cLevel, ZSTD_CONTENTSIZE_UNKNOWN, dictSize);
                ZSTD_frameParameters const fPar = { FUZ_rand(&lseed)&1 /* contentSizeFlag */,
                                                    !(FUZ_rand(&lseed)&3) /* contentChecksumFlag*/,
                                                    0 /*NodictID*/ };   /* note : since dictionary is fake, dictIDflag has no impact */
                ZSTD_parameters const p = FUZ_makeParams(cPar, fPar);
                CHECK_Z ( ZSTD_compressBegin_advanced(refCtx, dict, dictSize, p, 0) );
            }
            CHECK_Z( ZSTD_copyCCtx(ctx, refCtx, 0) );
        }

        {   U32 const nbChunks = (FUZ_rand(&lseed) & 127) + 2;
            U32 n;
            XXH64_state_t xxhState;
            XXH64_reset(&xxhState, 0);
            for (totalTestSize=0, cSize=0, n=0 ; n<nbChunks ; n++) {
                size_t const segmentSize = FUZ_randomLength(&lseed, maxSampleLog);
                size_t const segmentStart = FUZ_rand(&lseed) % (srcBufferSize - segmentSize);

                if (cBufferSize-cSize < ZSTD_compressBound(segmentSize)) break;   /* avoid invalid dstBufferTooSmall */
                if (totalTestSize+segmentSize > maxTestSize) break;

                {   size_t const compressResult = ZSTD_compressContinue(ctx, cBuffer+cSize, cBufferSize-cSize, srcBuffer+segmentStart, segmentSize);
                    CHECK (ZSTD_isError(compressResult), "multi-segments compression error : %s", ZSTD_getErrorName(compressResult));
                    cSize += compressResult;
                }
                XXH64_update(&xxhState, srcBuffer+segmentStart, segmentSize);
                memcpy(mirrorBuffer + totalTestSize, srcBuffer+segmentStart, segmentSize);
                totalTestSize += segmentSize;
            }

            {   size_t const flushResult = ZSTD_compressEnd(ctx, cBuffer+cSize, cBufferSize-cSize, NULL, 0);
                CHECK (ZSTD_isError(flushResult), "multi-segments epilogue error : %s", ZSTD_getErrorName(flushResult));
                cSize += flushResult;
            }
            crcOrig = XXH64_digest(&xxhState);
        }

        /* streaming decompression test */
        DISPLAYLEVEL(5, "fuzzer t%u: Bufferless streaming decompression test \n", testNb);
        /* ensure memory requirement is good enough (should always be true) */
        {   ZSTD_frameHeader zfh;
            CHECK( ZSTD_getFrameHeader(&zfh, cBuffer, ZSTD_FRAMEHEADERSIZE_MAX),
                  "ZSTD_getFrameHeader(): error retrieving frame information");
            {   size_t const roundBuffSize = ZSTD_decodingBufferSize_min(zfh.windowSize, zfh.frameContentSize);
                CHECK_Z(roundBuffSize);
                CHECK((roundBuffSize > totalTestSize) && (zfh.frameContentSize!=ZSTD_CONTENTSIZE_UNKNOWN),
                      "ZSTD_decodingBufferSize_min() requires more memory (%u) than necessary (%u)",
                      (unsigned)roundBuffSize, (unsigned)totalTestSize );
        }   }
        if (dictSize<8) dictSize=0, dict=NULL;   /* disable dictionary */
        CHECK_Z( ZSTD_decompressBegin_usingDict(dctx, dict, dictSize) );
        totalCSize = 0;
        totalGenSize = 0;
        while (totalCSize < cSize) {
            size_t const inSize = ZSTD_nextSrcSizeToDecompress(dctx);
            size_t const genSize = ZSTD_decompressContinue(dctx, dstBuffer+totalGenSize, dstBufferSize-totalGenSize, cBuffer+totalCSize, inSize);
            CHECK (ZSTD_isError(genSize), "ZSTD_decompressContinue error : %s", ZSTD_getErrorName(genSize));
            totalGenSize += genSize;
            totalCSize += inSize;
        }
        CHECK (ZSTD_nextSrcSizeToDecompress(dctx) != 0, "frame not fully decoded");
        CHECK (totalGenSize != totalTestSize, "streaming decompressed data : wrong size")
        CHECK (totalCSize != cSize, "compressed data should be fully read")
        {   U64 const crcDest = XXH64(dstBuffer, totalTestSize, 0);
            CHECK(crcOrig != crcDest, "streaming decompressed data corrupted (pos %u / %u)",
                (unsigned)findDiff(mirrorBuffer, dstBuffer, totalTestSize), (unsigned)totalTestSize);
        }
    }   /* for ( ; (testNb <= nbTests) */
    DISPLAY("\r%u fuzzer tests completed   \n", testNb-1);

_cleanup:
    ZSTD_freeCCtx(refCtx);
    ZSTD_freeCCtx(ctx);
    ZSTD_freeDCtx(dctx);
    free(cNoiseBuffer[0]);
    free(cNoiseBuffer[1]);
    free(cNoiseBuffer[2]);
    free(cNoiseBuffer[3]);
    free(cNoiseBuffer[4]);
    free(cBuffer);
    free(dstBuffer);
    free(mirrorBuffer);
    return result;

_output_error:
    result = 1;
    goto _cleanup;
}


/*_*******************************************************
*  Command line
*********************************************************/
static int FUZ_usage(const char* programName)
{
    DISPLAY( "Usage :\n");
    DISPLAY( "      %s [args]\n", programName);
    DISPLAY( "\n");
    DISPLAY( "Arguments :\n");
    DISPLAY( " -i#    : Number of tests (default:%i)\n", nbTestsDefault);
    DISPLAY( " -T#    : Max duration to run for. Overrides number of tests. (e.g. -T1m or -T60s for one minute)\n");
    DISPLAY( " -s#    : Select seed (default:prompt user)\n");
    DISPLAY( " -t#    : Select starting test number (default:0)\n");
    DISPLAY( " -P#    : Select compressibility in %% (default:%i%%)\n", FUZ_compressibility_default);
    DISPLAY( " -v     : verbose\n");
    DISPLAY( " -p     : pause at the end\n");
    DISPLAY( " -h     : display help and exit\n");
    return 0;
}

/*! readU32FromChar() :
    @return : unsigned integer value read from input in `char` format
    allows and interprets K, KB, KiB, M, MB and MiB suffix.
    Will also modify `*stringPtr`, advancing it to position where it stopped reading.
    Note : function result can overflow if digit string > MAX_UINT */
static unsigned readU32FromChar(const char** stringPtr)
{
    unsigned result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9'))
        result *= 10, result += **stringPtr - '0', (*stringPtr)++ ;
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        result <<= 10;
        if (**stringPtr=='M') result <<= 10;
        (*stringPtr)++ ;
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    return result;
}

/** longCommandWArg() :
 *  check if *stringPtr is the same as longCommand.
 *  If yes, @return 1 and advances *stringPtr to the position which immediately follows longCommand.
 *  @return 0 and doesn't modify *stringPtr otherwise.
 */
static int longCommandWArg(const char** stringPtr, const char* longCommand)
{
    size_t const comSize = strlen(longCommand);
    int const result = !strncmp(*stringPtr, longCommand, comSize);
    if (result) *stringPtr += comSize;
    return result;
}

int main(int argc, const char** argv)
{
    U32 seed = 0;
    int seedset = 0;
    int argNb;
    int nbTests = nbTestsDefault;
    int testNb = 0;
    int proba = FUZ_compressibility_default;
    double probfloat;
    int result = 0;
    U32 mainPause = 0;
    U32 maxDuration = 0;
    int bigTests = 1;
    int longTests = 0;
    U32 memTestsOnly = 0;
    const char* const programName = argv[0];

    /* Check command line */
    for (argNb=1; argNb<argc; argNb++) {
        const char* argument = argv[argNb];
        if(!argument) continue;   /* Protection if argument empty */

        /* Handle commands. Aggregated commands are allowed */
        if (argument[0]=='-') {

            if (longCommandWArg(&argument, "--memtest=")) { memTestsOnly = readU32FromChar(&argument); continue; }

            if (!strcmp(argument, "--memtest")) { memTestsOnly=1; continue; }
            if (!strcmp(argument, "--no-big-tests")) { bigTests=0; continue; }
            if (!strcmp(argument, "--long-tests")) { longTests=1; continue; }
            if (!strcmp(argument, "--no-long-tests")) { longTests=0; continue; }

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

                case 'i':
                    argument++; maxDuration = 0;
                    nbTests = (int)readU32FromChar(&argument);
                    break;

                case 'T':
                    argument++;
                    nbTests = 0;
                    maxDuration = readU32FromChar(&argument);
                    if (*argument=='s') argument++;   /* seconds */
                    if (*argument=='m') maxDuration *= 60, argument++;   /* minutes */
                    if (*argument=='n') argument++;
                    break;

                case 's':
                    argument++;
                    seedset = 1;
                    seed = readU32FromChar(&argument);
                    break;

                case 't':
                    argument++;
                    testNb = (int)readU32FromChar(&argument);
                    break;

                case 'P':   /* compressibility % */
                    argument++;
                    proba = (int)readU32FromChar(&argument);
                    if (proba>100) proba = 100;
                    break;

                default:
                    return (FUZ_usage(programName), 1);
    }   }   }   }   /* for (argNb=1; argNb<argc; argNb++) */

    /* Get Seed */
    DISPLAY("Starting zstd tester (%i-bits, %s)\n", (int)(sizeof(size_t)*8), ZSTD_VERSION_STRING);

    if (!seedset) {
        time_t const t = time(NULL);
        U32 const h = XXH32(&t, sizeof(t), 1);
        seed = h % 10000;
    }

    DISPLAY("Seed = %u\n", (unsigned)seed);
    if (proba!=FUZ_compressibility_default) DISPLAY("Compressibility : %i%%\n", proba);

    probfloat = ((double)proba) / 100;

    if (memTestsOnly) {
        g_displayLevel = MAX(3, g_displayLevel);
        return FUZ_mallocTests(seed, probfloat, memTestsOnly);
    }

    if (nbTests < testNb) nbTests = testNb;

    if (testNb==0) {
        result = basicUnitTests(0, probfloat);  /* constant seed for predictability */

        if (!result && longTests) {
            result = longUnitTests(0, probfloat);
        }
    }
    if (!result)
        result = fuzzerTests(seed, nbTests, testNb, maxDuration, ((double)proba) / 100, bigTests);
    if (mainPause) {
        int unused;
        DISPLAY("Press Enter \n");
        unused = getchar();
        (void)unused;
    }
    return result;
}
