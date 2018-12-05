/*
 * Copyright (c) 2016-present, Yann Collet, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#if defined (__cplusplus)
extern "C" {
#endif

#ifndef BENCH_H_121279284357
#define BENCH_H_121279284357

/* ===  Dependencies  === */
#include <stddef.h>   /* size_t */
#define ZSTD_STATIC_LINKING_ONLY   /* ZSTD_compressionParameters */
#include "zstd.h"     /* ZSTD_compressionParameters */


/* ===  Constants  === */

#define MB_UNIT 1000000


/* ===  Benchmark functions  === */

/* Creates a variant `typeName`, able to express "error or valid result".
 * Functions with return type `typeName`
 * must first check if result is valid, using BMK_isSuccessful_*(),
 * and only then can extract `baseType`.
 */
#define VARIANT_ERROR_RESULT(baseType, variantName)  \
                                             \
typedef struct {                             \
    baseType internal_never_use_directly;    \
    int tag;                                 \
} variantName


typedef struct {
    size_t cSize;
    unsigned long long cSpeed;   /* bytes / sec */
    unsigned long long dSpeed;
    size_t cMem;                 /* memory usage during compression */
} BMK_benchResult_t;

VARIANT_ERROR_RESULT(BMK_benchResult_t, BMK_benchOutcome_t);

/* check first if the return structure represents an error or a valid result */
int BMK_isSuccessful_benchOutcome(BMK_benchOutcome_t outcome);

/* extract result from variant type.
 * note : this function will abort() program execution if result is not valid
 *        check result validity first, by using BMK_isSuccessful_benchOutcome()
 */
BMK_benchResult_t BMK_extract_benchResult(BMK_benchOutcome_t outcome);


/*! BMK_benchFiles() -- called by zstdcli */
/*  Loads files from fileNamesTable into memory,
 *  and an optional dictionary from dictFileName (can be NULL),
 *  then uses benchMem().
 *  fileNamesTable - name of files to benchmark.
 *  nbFiles - number of files (size of fileNamesTable), must be > 0.
 *  dictFileName - name of dictionary file to load.
 *  cLevel - compression level to benchmark, errors if invalid.
 *  compressionParams - advanced compression Parameters.
 *  displayLevel - what gets printed:
 *      0 : no display;
 *      1 : errors;
 *      2 : + result + interaction + warnings;
 *      3 : + information;
 *      4 : + debug
 * @return:
 *      a variant, which expresses either an error, or a valid result.
 *      Use BMK_isSuccessful_benchOutcome() to check if function was successful.
 *      If yes, extract the valid result with BMK_extract_benchResult(),
 *      it will contain :
 *          .cSpeed: compression speed in bytes per second,
 *          .dSpeed: decompression speed in bytes per second,
 *          .cSize : compressed size, in bytes
 *          .cMem  : memory budget required for the compression context
 */
BMK_benchOutcome_t BMK_benchFiles(
                   const char* const * fileNamesTable, unsigned nbFiles,
                   const char* dictFileName,
                   int cLevel, const ZSTD_compressionParameters* compressionParams,
                   int displayLevel);


typedef enum {
    BMK_both = 0,
    BMK_decodeOnly = 1,
    BMK_compressOnly = 2
} BMK_mode_t;

typedef struct {
    BMK_mode_t mode;            /* 0: all, 1: compress only 2: decode only */
    unsigned nbSeconds;         /* default timing is in nbSeconds */
    size_t blockSize;           /* Maximum size of each block*/
    unsigned nbWorkers;         /* multithreading */
    unsigned realTime;          /* real time priority */
    int additionalParam;        /* used by python speed benchmark */
    unsigned ldmFlag;           /* enables long distance matching */
    unsigned ldmMinMatch;       /* below: parameters for long distance matching, see zstd.1.md */
    unsigned ldmHashLog;
    unsigned ldmBucketSizeLog;
    unsigned ldmHashEveryLog;
} BMK_advancedParams_t;

/* returns default parameters used by nonAdvanced functions */
BMK_advancedParams_t BMK_initAdvancedParams(void);

/*! BMK_benchFilesAdvanced():
 *  Same as BMK_benchFiles(),
 *  with more controls, provided through advancedParams_t structure */
BMK_benchOutcome_t BMK_benchFilesAdvanced(
                   const char* const * fileNamesTable, unsigned nbFiles,
                   const char* dictFileName,
                   int cLevel, const ZSTD_compressionParameters* compressionParams,
                   int displayLevel, const BMK_advancedParams_t* adv);

/*! BMK_syntheticTest() -- called from zstdcli */
/*  Generates a sample with datagen, using compressibility argument */
/*  cLevel - compression level to benchmark, errors if invalid
 *  compressibility - determines compressibility of sample
 *  compressionParams - basic compression Parameters
 *  displayLevel - see benchFiles
 *  adv - see advanced_Params_t
 * @return:
 *      a variant, which expresses either an error, or a valid result.
 *      Use BMK_isSuccessful_benchOutcome() to check if function was successful.
 *      If yes, extract the valid result with BMK_extract_benchResult(),
 *      it will contain :
 *          .cSpeed: compression speed in bytes per second,
 *          .dSpeed: decompression speed in bytes per second,
 *          .cSize : compressed size, in bytes
 *          .cMem  : memory budget required for the compression context
 */
BMK_benchOutcome_t BMK_syntheticTest(
                              int cLevel, double compressibility,
                              const ZSTD_compressionParameters* compressionParams,
                              int displayLevel, const BMK_advancedParams_t* adv);



/* ===  Benchmark Zstandard in a memory-to-memory scenario  === */

/** BMK_benchMem() -- core benchmarking function, called in paramgrill
 *  applies ZSTD_compress_generic() and ZSTD_decompress_generic() on data in srcBuffer
 *  with specific compression parameters provided by other arguments using benchFunction
 *  (cLevel, comprParams + adv in advanced Mode) */
/*  srcBuffer - data source, expected to be valid compressed data if in Decode Only Mode
 *  srcSize - size of data in srcBuffer
 *  fileSizes - srcBuffer is considered cut into 1+ segments, to compress separately.
 *              note : sum(fileSizes) must be == srcSize.  (<== ensure it's properly checked)
 *  nbFiles - nb of segments
 *  cLevel - compression level
 *  comprParams - basic compression parameters
 *  dictBuffer - a dictionary if used, null otherwise
 *  dictBufferSize - size of dictBuffer, 0 otherwise
 *  diplayLevel - see BMK_benchFiles
 *  displayName - name used by display
 * @return:
 *      a variant, which expresses either an error, or a valid result.
 *      Use BMK_isSuccessful_benchOutcome() to check if function was successful.
 *      If yes, extract the valid result with BMK_extract_benchResult(),
 *      it will contain :
 *          .cSpeed: compression speed in bytes per second,
 *          .dSpeed: decompression speed in bytes per second,
 *          .cSize : compressed size, in bytes
 *          .cMem  : memory budget required for the compression context
 */
BMK_benchOutcome_t BMK_benchMem(const void* srcBuffer, size_t srcSize,
                        const size_t* fileSizes, unsigned nbFiles,
                        int cLevel, const ZSTD_compressionParameters* comprParams,
                        const void* dictBuffer, size_t dictBufferSize,
                        int displayLevel, const char* displayName);

/* BMK_benchMemAdvanced() : same as BMK_benchMem()
 * with following additional options :
 * dstBuffer - destination buffer to write compressed output in, NULL if none provided.
 * dstCapacity - capacity of destination buffer, give 0 if dstBuffer = NULL
 * adv = see advancedParams_t
 */
BMK_benchOutcome_t BMK_benchMemAdvanced(const void* srcBuffer, size_t srcSize,
                        void* dstBuffer, size_t dstCapacity,
                        const size_t* fileSizes, unsigned nbFiles,
                        int cLevel, const ZSTD_compressionParameters* comprParams,
                        const void* dictBuffer, size_t dictBufferSize,
                        int displayLevel, const char* displayName,
                        const BMK_advancedParams_t* adv);



/* ====  Benchmarking any function, iterated on a set of blocks  ==== */

typedef struct {
    unsigned long long nanoSecPerRun;  /* time per iteration */
    size_t sumOfReturn;       /* sum of return values */
} BMK_runTime_t;

VARIANT_ERROR_RESULT(BMK_runTime_t, BMK_runOutcome_t);

/* check first if the return structure represents an error or a valid result */
int BMK_isSuccessful_runOutcome(BMK_runOutcome_t outcome);

/* extract result from variant type.
 * note : this function will abort() program execution if result is not valid
 *        check result validity first, by using BMK_isSuccessful_runOutcome()
 */
BMK_runTime_t BMK_extract_runTime(BMK_runOutcome_t outcome);



typedef size_t (*BMK_benchFn_t)(const void* src, size_t srcSize, void* dst, size_t dstCapacity, void* customPayload);
typedef size_t (*BMK_initFn_t)(void* initPayload);


/* BMK_benchFunction() :
 * This function times the execution of 2 argument functions, benchFn and initFn  */

/* benchFn - (*benchFn)(srcBuffers[i], srcSizes[i], dstBuffers[i], dstCapacities[i], benchPayload)
 *      is run nbLoops times
 * initFn - (*initFn)(initPayload) is run once per benchmark, at the beginning.
 *      This argument can be NULL, in which case nothing is run.
 * blockCount - number of blocks. Size of all array parameters : srcBuffers, srcSizes, dstBuffers, dstCapacities, blockResults
 * srcBuffers - an array of buffers to be operated on by benchFn
 * srcSizes - an array of the sizes of above buffers
 * dstBuffers - an array of buffers to be written into by benchFn
 * dstCapacities - an array of the capacities of above buffers
 * blockResults - Optional: store the return value of benchFn for each block. Use NULL if this result is not requested.
 * nbLoops - defines number of times benchFn is run.
 * @return: a variant, which express either an error, or can generate a valid BMK_runTime_t result.
 *          Use BMK_isSuccessful_runOutcome() to check if function was successful.
 *          If yes, extract the result with BMK_extract_runTime(),
 *          it will contain :
 *              .sumOfReturn : the sum of all return values of benchFn through all of blocks
 *              .nanoSecPerRun : time per run of benchFn + (time for initFn / nbLoops)
 *          .sumOfReturn is generally intended for functions which return a # of bytes written into dstBuffer,
 *              in which case, this value will be the total amount of bytes written into dstBuffer.
 */
BMK_runOutcome_t BMK_benchFunction(
                        BMK_benchFn_t benchFn, void* benchPayload,
                        BMK_initFn_t initFn, void* initPayload,
                        size_t blockCount,
                        const void *const * srcBuffers, const size_t* srcSizes,
                        void *const * dstBuffers, const size_t* dstCapacities,
                        size_t* blockResults,
                        unsigned nbLoops);



/* ====  Benchmark any function, providing intermediate results  ==== */

/* state information tracking benchmark session */
typedef struct BMK_timedFnState_s BMK_timedFnState_t;

/* BMK_createTimedFnState() and BMK_resetTimedFnState() :
 * Create/Set BMK_timedFnState_t for next benchmark session,
 * which shall last a minimum of total_ms milliseconds,
 * producing intermediate results, paced at interval of (approximately) run_ms.
 */
BMK_timedFnState_t* BMK_createTimedFnState(unsigned total_ms, unsigned run_ms);
void BMK_resetTimedFnState(BMK_timedFnState_t* timedFnState, unsigned total_ms, unsigned run_ms);
void BMK_freeTimedFnState(BMK_timedFnState_t* state);


/* Tells if duration of all benchmark runs has exceeded total_ms
 */
int BMK_isCompleted_TimedFn(const BMK_timedFnState_t* timedFnState);


/* BMK_benchTimedFn() :
 * Similar to BMK_benchFunction(), most arguments being identical.
 * Automatically determines `nbLoops` so that each result is regularly produced at interval of about run_ms.
 * Note : minimum `nbLoops` is 1, therefore a run may last more than run_ms, and possibly even more than total_ms.
 * Usage - initialize timedFnState, select benchmark duration (total_ms) and each measurement duration (run_ms)
 *         call BMK_benchTimedFn() repetitively, each measurement is supposed to last about run_ms
 *         Check if total time budget is spent or exceeded, using BMK_isCompleted_TimedFn()
 */
BMK_runOutcome_t BMK_benchTimedFn(
                    BMK_timedFnState_t* timedFnState,
                    BMK_benchFn_t benchFn, void* benchPayload,
                    BMK_initFn_t initFn, void* initPayload,
                    size_t blockCount,
                    const void *const * srcBlockBuffers, const size_t* srcBlockSizes,
                    void *const * dstBlockBuffers, const size_t* dstBlockCapacities,
                    size_t* blockResults);





#endif   /* BENCH_H_121279284357 */

#if defined (__cplusplus)
}
#endif
