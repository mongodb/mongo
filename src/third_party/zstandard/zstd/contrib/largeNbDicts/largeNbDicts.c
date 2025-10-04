/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/* largeNbDicts
 * This is a benchmark test tool
 * dedicated to the specific case of dictionary decompression
 * using a very large nb of dictionaries
 * thus suffering latency from lots of cache misses.
 * It's created in a bid to investigate performance and find optimizations. */


/*---  Dependencies  ---*/

#include <stddef.h>   /* size_t */
#include <stdlib.h>   /* malloc, free, abort, qsort*/
#include <stdio.h>    /* fprintf */
#include <limits.h>   /* UINT_MAX */
#include <assert.h>   /* assert */

#include "util.h"
#include "benchfn.h"
#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"
#include "zdict.h"


/*---  Constants  --- */

#define KB  *(1<<10)
#define MB  *(1<<20)

#define BLOCKSIZE_DEFAULT 0  /* no slicing into blocks */
#define DICTSIZE  (4 KB)
#define CLEVEL_DEFAULT 3
#define DICT_LOAD_METHOD ZSTD_dlm_byCopy

#define BENCH_TIME_DEFAULT_S   6
#define RUN_TIME_DEFAULT_MS    1000
#define BENCH_TIME_DEFAULT_MS (BENCH_TIME_DEFAULT_S * RUN_TIME_DEFAULT_MS)

#define DISPLAY_LEVEL_DEFAULT 3

#define BENCH_SIZE_MAX (1200 MB)


/*---  Macros  ---*/

#define CONTROL(c)   { if (!(c)) abort(); }
#undef MIN
#define MIN(a,b)     ((a) < (b) ? (a) : (b))


/*---  Display Macros  ---*/

#define DISPLAY(...)         fprintf(stdout, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) { if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); } }
static int g_displayLevel = DISPLAY_LEVEL_DEFAULT;   /* 0 : no display,  1: errors,  2 : + result + interaction + warnings,  3 : + progression,  4 : + information */


/*---  buffer_t  ---*/

typedef struct {
    void* ptr;
    size_t size;
    size_t capacity;
} buffer_t;

static const buffer_t kBuffNull = { NULL, 0, 0 };

/* @return : kBuffNull if any error */
static buffer_t createBuffer(size_t capacity)
{
    assert(capacity > 0);
    void* const ptr = malloc(capacity);
    if (ptr==NULL) return kBuffNull;

    buffer_t buffer;
    buffer.ptr = ptr;
    buffer.capacity = capacity;
    buffer.size = 0;
    return buffer;
}

static void freeBuffer(buffer_t buff)
{
    free(buff.ptr);
}


static void fillBuffer_fromHandle(buffer_t* buff, FILE* f)
{
    size_t const readSize = fread(buff->ptr, 1, buff->capacity, f);
    buff->size = readSize;
}


/* @return : kBuffNull if any error */
static buffer_t createBuffer_fromFile(const char* fileName)
{
    U64 const fileSize = UTIL_getFileSize(fileName);
    size_t const bufferSize = (size_t) fileSize;

    if (fileSize == UTIL_FILESIZE_UNKNOWN) return kBuffNull;
    assert((U64)bufferSize == fileSize);   /* check overflow */

    {   FILE* const f = fopen(fileName, "rb");
        if (f == NULL) return kBuffNull;

        buffer_t buff = createBuffer(bufferSize);
        CONTROL(buff.ptr != NULL);

        fillBuffer_fromHandle(&buff, f);
        CONTROL(buff.size == buff.capacity);

        fclose(f);   /* do nothing specific if fclose() fails */
        return buff;
    }
}


/* @return : kBuffNull if any error */
static buffer_t
createDictionaryBuffer(const char* dictionaryName,
                       const void* srcBuffer,
                       const size_t* srcBlockSizes, size_t nbBlocks,
                       size_t requestedDictSize)
{
    if (dictionaryName) {
        DISPLAYLEVEL(3, "loading dictionary %s \n", dictionaryName);
        return createBuffer_fromFile(dictionaryName);  /* note : result might be kBuffNull */

    } else {

        DISPLAYLEVEL(3, "creating dictionary, of target size %u bytes \n",
                        (unsigned)requestedDictSize);
        void* const dictBuffer = malloc(requestedDictSize);
        CONTROL(dictBuffer != NULL);

        assert(nbBlocks <= UINT_MAX);
        size_t const dictSize = ZDICT_trainFromBuffer(dictBuffer, requestedDictSize,
                                                      srcBuffer,
                                                      srcBlockSizes, (unsigned)nbBlocks);
        CONTROL(!ZSTD_isError(dictSize));

        buffer_t result;
        result.ptr = dictBuffer;
        result.capacity = requestedDictSize;
        result.size = dictSize;
        return result;
    }
}

/*! BMK_loadFiles() :
 *  Loads `buffer`, with content from files listed within `fileNamesTable`.
 *  Fills `buffer` entirely.
 * @return : 0 on success, !=0 on error */
static int loadFiles(void* buffer, size_t bufferSize,
                     size_t* fileSizes,
                     const char* const * fileNamesTable, unsigned nbFiles)
{
    size_t pos = 0, totalSize = 0;

    for (unsigned n=0; n<nbFiles; n++) {
        U64 fileSize = UTIL_getFileSize(fileNamesTable[n]);
        if (UTIL_isDirectory(fileNamesTable[n])) {
            fileSizes[n] = 0;
            continue;
        }
        if (fileSize == UTIL_FILESIZE_UNKNOWN) {
            fileSizes[n] = 0;
            continue;
        }

        FILE* const f = fopen(fileNamesTable[n], "rb");
        assert(f!=NULL);

        assert(pos <= bufferSize);
        assert(fileSize <= bufferSize - pos);

        {   size_t const readSize = fread(((char*)buffer)+pos, 1, (size_t)fileSize, f);
            assert(readSize == fileSize);
            pos += readSize;
        }
        fileSizes[n] = (size_t)fileSize;
        totalSize += (size_t)fileSize;
        fclose(f);
    }

    assert(totalSize == bufferSize);
    return 0;
}



/*---  slice_collection_t  ---*/

typedef struct {
    void** slicePtrs;
    size_t* capacities;
    size_t nbSlices;
} slice_collection_t;

static const slice_collection_t kNullCollection = { NULL, NULL, 0 };

static void freeSliceCollection(slice_collection_t collection)
{
    free(collection.slicePtrs);
    free(collection.capacities);
}

/* shrinkSizes() :
 * downsizes sizes of slices within collection, according to `newSizes`.
 * every `newSizes` entry must be <= than its corresponding collection size */
void shrinkSizes(slice_collection_t collection,
                 const size_t* newSizes)  /* presumed same size as collection */
{
    size_t const nbSlices = collection.nbSlices;
    for (size_t blockNb = 0; blockNb < nbSlices; blockNb++) {
        assert(newSizes[blockNb] <= collection.capacities[blockNb]);
        collection.capacities[blockNb] = newSizes[blockNb];
    }
}


/* splitSlices() :
 * nbSlices : if == 0, nbSlices is automatically determined from srcSlices and blockSize.
 *            otherwise, creates exactly nbSlices slices,
 *            by either truncating input (when smaller)
 *            or repeating input from beginning */
static slice_collection_t
splitSlices(slice_collection_t srcSlices, size_t blockSize, size_t nbSlices)
{
    if (blockSize==0) blockSize = (size_t)(-1);   /* means "do not cut" */
    size_t nbSrcBlocks = 0;
    for (size_t ssnb=0; ssnb < srcSlices.nbSlices; ssnb++) {
        size_t pos = 0;
        while (pos <= srcSlices.capacities[ssnb]) {
            nbSrcBlocks++;
            pos += blockSize;
        }
    }

    if (nbSlices == 0) nbSlices = nbSrcBlocks;

    void** const sliceTable = (void**)malloc(nbSlices * sizeof(*sliceTable));
    size_t* const capacities = (size_t*)malloc(nbSlices * sizeof(*capacities));
    if (sliceTable == NULL || capacities == NULL) {
        free(sliceTable);
        free(capacities);
        return kNullCollection;
    }

    size_t ssnb = 0;
    for (size_t sliceNb=0; sliceNb < nbSlices; ) {
        ssnb = (ssnb + 1) % srcSlices.nbSlices;
        size_t pos = 0;
        char* const ptr = (char*)srcSlices.slicePtrs[ssnb];
        while (pos < srcSlices.capacities[ssnb] && sliceNb < nbSlices) {
            size_t const size = MIN(blockSize, srcSlices.capacities[ssnb] - pos);
            sliceTable[sliceNb] = ptr + pos;
            capacities[sliceNb] = size;
            sliceNb++;
            pos += blockSize;
        }
    }

    slice_collection_t result;
    result.nbSlices = nbSlices;
    result.slicePtrs = sliceTable;
    result.capacities = capacities;
    return result;
}


static size_t sliceCollection_totalCapacity(slice_collection_t sc)
{
    size_t totalSize = 0;
    for (size_t n=0; n<sc.nbSlices; n++)
        totalSize += sc.capacities[n];
    return totalSize;
}


/* ---  buffer collection  --- */

typedef struct {
    buffer_t buffer;
    slice_collection_t slices;
} buffer_collection_t;


static void freeBufferCollection(buffer_collection_t bc)
{
    freeBuffer(bc.buffer);
    freeSliceCollection(bc.slices);
}


static buffer_collection_t
createBufferCollection_fromSliceCollectionSizes(slice_collection_t sc)
{
    size_t const bufferSize = sliceCollection_totalCapacity(sc);

    buffer_t buffer = createBuffer(bufferSize);
    CONTROL(buffer.ptr != NULL);

    size_t const nbSlices = sc.nbSlices;
    void** const slices = (void**)malloc(nbSlices * sizeof(*slices));
    CONTROL(slices != NULL);

    size_t* const capacities = (size_t*)malloc(nbSlices * sizeof(*capacities));
    CONTROL(capacities != NULL);

    char* const ptr = (char*)buffer.ptr;
    size_t pos = 0;
    for (size_t n=0; n < nbSlices; n++) {
        capacities[n] = sc.capacities[n];
        slices[n] = ptr + pos;
        pos += capacities[n];
    }

    buffer_collection_t result;
    result.buffer = buffer;
    result.slices.nbSlices = nbSlices;
    result.slices.capacities = capacities;
    result.slices.slicePtrs = slices;
    return result;
}

static buffer_collection_t
createBufferCollection_fromSliceCollection(slice_collection_t sc)
{
    size_t const bufferSize = sliceCollection_totalCapacity(sc);

    buffer_t buffer = createBuffer(bufferSize);
    CONTROL(buffer.ptr != NULL);

    size_t const nbSlices = sc.nbSlices;
    void** const slices = (void**)malloc(nbSlices * sizeof(*slices));
    CONTROL(slices != NULL);

    size_t* const capacities = (size_t*)malloc(nbSlices * sizeof(*capacities));
    CONTROL(capacities != NULL);

    char* const ptr = (char*)buffer.ptr;
    size_t pos = 0;
    for (size_t n=0; n < nbSlices; n++) {
        capacities[n] = sc.capacities[n];
        slices[n] = ptr + pos;
        pos += capacities[n];
    }

    for (size_t i = 0; i < nbSlices; i++) {
        memcpy(slices[i], sc.slicePtrs[i], sc.capacities[i]);
        capacities[i] = sc.capacities[i];
    }

    buffer_collection_t result;
    result.buffer = buffer;
    result.slices.nbSlices = nbSlices;
    result.slices.capacities = capacities;
    result.slices.slicePtrs = slices;

    return result;
}

/* @return : kBuffNull if any error */
static buffer_collection_t
createBufferCollection_fromFiles(const char* const * fileNamesTable, unsigned nbFiles)
{
    U64 const totalSizeToLoad = UTIL_getTotalFileSize(fileNamesTable, nbFiles);
    assert(totalSizeToLoad != UTIL_FILESIZE_UNKNOWN);
    assert(totalSizeToLoad <= BENCH_SIZE_MAX);
    size_t const loadedSize = (size_t)totalSizeToLoad;
    assert(loadedSize > 0);
    void* const srcBuffer = malloc(loadedSize);
    assert(srcBuffer != NULL);

    assert(nbFiles > 0);
    size_t* const fileSizes = (size_t*)calloc(nbFiles, sizeof(*fileSizes));
    assert(fileSizes != NULL);

    /* Load input buffer */
    int const errorCode = loadFiles(srcBuffer, loadedSize,
                                    fileSizes,
                                    fileNamesTable, nbFiles);
    assert(errorCode == 0);

    void** sliceTable = (void**)malloc(nbFiles * sizeof(*sliceTable));
    assert(sliceTable != NULL);

    char* const ptr = (char*)srcBuffer;
    size_t pos = 0;
    unsigned fileNb = 0;
    for ( ; (pos < loadedSize) && (fileNb < nbFiles); fileNb++) {
        sliceTable[fileNb] = ptr + pos;
        pos += fileSizes[fileNb];
    }
    assert(pos == loadedSize);
    assert(fileNb == nbFiles);


    buffer_t buffer;
    buffer.ptr = srcBuffer;
    buffer.capacity = loadedSize;
    buffer.size = loadedSize;

    slice_collection_t slices;
    slices.slicePtrs = sliceTable;
    slices.capacities = fileSizes;
    slices.nbSlices = nbFiles;

    buffer_collection_t bc;
    bc.buffer = buffer;
    bc.slices = slices;
    return bc;
}




/*---  ddict_collection_t  ---*/

typedef struct {
    ZSTD_DDict** ddicts;
    size_t nbDDict;
} ddict_collection_t;

typedef struct {
    ZSTD_CDict** cdicts;
    size_t nbCDict;
} cdict_collection_t;

static const cdict_collection_t kNullCDictCollection = { NULL, 0 };

static void freeCDictCollection(cdict_collection_t cdictc)
{
    for (size_t dictNb=0; dictNb < cdictc.nbCDict; dictNb++) {
        ZSTD_freeCDict(cdictc.cdicts[dictNb]);
    }
    free(cdictc.cdicts);
}

/* returns .buffers=NULL if operation fails */
static cdict_collection_t createCDictCollection(const void* dictBuffer, size_t dictSize, size_t nbCDict, ZSTD_dictContentType_e dictContentType, ZSTD_CCtx_params* cctxParams)
{
    ZSTD_CDict** const cdicts = malloc(nbCDict * sizeof(ZSTD_CDict*));
    if (cdicts==NULL) return kNullCDictCollection;
    for (size_t dictNb=0; dictNb < nbCDict; dictNb++) {
        cdicts[dictNb] = ZSTD_createCDict_advanced2(dictBuffer, dictSize, DICT_LOAD_METHOD, dictContentType, cctxParams, ZSTD_defaultCMem);
        CONTROL(cdicts[dictNb] != NULL);
    }
    cdict_collection_t cdictc;
    cdictc.cdicts = cdicts;
    cdictc.nbCDict = nbCDict;
    return cdictc;
}

static const ddict_collection_t kNullDDictCollection = { NULL, 0 };

static void freeDDictCollection(ddict_collection_t ddictc)
{
    for (size_t dictNb=0; dictNb < ddictc.nbDDict; dictNb++) {
        ZSTD_freeDDict(ddictc.ddicts[dictNb]);
    }
    free(ddictc.ddicts);
}

/* returns .buffers=NULL if operation fails */
static ddict_collection_t createDDictCollection(const void* dictBuffer, size_t dictSize, size_t nbDDict)
{
    ZSTD_DDict** const ddicts = malloc(nbDDict * sizeof(ZSTD_DDict*));
    assert(ddicts != NULL);
    if (ddicts==NULL) return kNullDDictCollection;
    for (size_t dictNb=0; dictNb < nbDDict; dictNb++) {
        ddicts[dictNb] = ZSTD_createDDict(dictBuffer, dictSize);
        assert(ddicts[dictNb] != NULL);
    }
    ddict_collection_t ddictc;
    ddictc.ddicts = ddicts;
    ddictc.nbDDict = nbDDict;
    return ddictc;
}


/* mess with addresses, so that linear scanning dictionaries != linear address scanning */
void shuffleCDictionaries(cdict_collection_t dicts)
{
    size_t const nbDicts = dicts.nbCDict;
    for (size_t r=0; r<nbDicts; r++) {
        size_t const d = (size_t)rand() % nbDicts;
        ZSTD_CDict* tmpd = dicts.cdicts[d];
        dicts.cdicts[d] = dicts.cdicts[r];
        dicts.cdicts[r] = tmpd;
    }
    for (size_t r=0; r<nbDicts; r++) {
        size_t const d1 = (size_t)rand() % nbDicts;
        size_t const d2 = (size_t)rand() % nbDicts;
        ZSTD_CDict* tmpd = dicts.cdicts[d1];
        dicts.cdicts[d1] = dicts.cdicts[d2];
        dicts.cdicts[d2] = tmpd;
    }
}

/* mess with addresses, so that linear scanning dictionaries != linear address scanning */
void shuffleDDictionaries(ddict_collection_t dicts)
{
    size_t const nbDicts = dicts.nbDDict;
    for (size_t r=0; r<nbDicts; r++) {
        size_t const d = (size_t)rand() % nbDicts;
        ZSTD_DDict* tmpd = dicts.ddicts[d];
        dicts.ddicts[d] = dicts.ddicts[r];
        dicts.ddicts[r] = tmpd;
    }
    for (size_t r=0; r<nbDicts; r++) {
        size_t const d1 = (size_t)rand() % nbDicts;
        size_t const d2 = (size_t)rand() % nbDicts;
        ZSTD_DDict* tmpd = dicts.ddicts[d1];
        dicts.ddicts[d1] = dicts.ddicts[d2];
        dicts.ddicts[d2] = tmpd;
    }
}


/* ---   Compression  --- */

/* compressBlocks() :
 * @return : total compressed size of all blocks,
 *        or 0 if error.
 */
static size_t compressBlocks(size_t* cSizes,   /* optional (can be NULL). If present, must contain at least nbBlocks fields */
                             slice_collection_t dstBlockBuffers,
                             slice_collection_t srcBlockBuffers,
                             ZSTD_CDict* cdict, int cLevel)
{
    size_t const nbBlocks = srcBlockBuffers.nbSlices;
    assert(dstBlockBuffers.nbSlices == srcBlockBuffers.nbSlices);

    ZSTD_CCtx* const cctx = ZSTD_createCCtx();
    assert(cctx != NULL);

    size_t totalCSize = 0;
    for (size_t blockNb=0; blockNb < nbBlocks; blockNb++) {
        size_t cBlockSize;
        if (cdict == NULL) {
            cBlockSize = ZSTD_compressCCtx(cctx,
                            dstBlockBuffers.slicePtrs[blockNb], dstBlockBuffers.capacities[blockNb],
                            srcBlockBuffers.slicePtrs[blockNb], srcBlockBuffers.capacities[blockNb],
                            cLevel);
        } else {
            cBlockSize = ZSTD_compress_usingCDict(cctx,
                            dstBlockBuffers.slicePtrs[blockNb], dstBlockBuffers.capacities[blockNb],
                            srcBlockBuffers.slicePtrs[blockNb], srcBlockBuffers.capacities[blockNb],
                            cdict);
        }
        CONTROL(!ZSTD_isError(cBlockSize));
        if (cSizes) cSizes[blockNb] = cBlockSize;
        totalCSize += cBlockSize;
    }
    return totalCSize;
}


/* ---  Benchmark  --- */

typedef struct {
    ZSTD_CCtx* cctx;
    size_t nbDicts;
    size_t dictNb;
    cdict_collection_t dictionaries;
} compressInstructions;

compressInstructions createCompressInstructions(cdict_collection_t dictionaries, ZSTD_CCtx_params* cctxParams)
{
    compressInstructions ci;
    ci.cctx = ZSTD_createCCtx();
    CONTROL(ci.cctx != NULL);
    if (cctxParams)
      ZSTD_CCtx_setParametersUsingCCtxParams(ci.cctx, cctxParams);
    ci.nbDicts = dictionaries.nbCDict;
    ci.dictNb = 0;
    ci.dictionaries = dictionaries;
    return ci;
}

void freeCompressInstructions(compressInstructions ci)
{
    ZSTD_freeCCtx(ci.cctx);
}

typedef struct {
    ZSTD_DCtx* dctx;
    size_t nbDicts;
    size_t dictNb;
    ddict_collection_t dictionaries;
} decompressInstructions;

decompressInstructions createDecompressInstructions(ddict_collection_t dictionaries)
{
    decompressInstructions di;
    di.dctx = ZSTD_createDCtx();
    assert(di.dctx != NULL);
    di.nbDicts = dictionaries.nbDDict;
    di.dictNb = 0;
    di.dictionaries = dictionaries;
    return di;
}

void freeDecompressInstructions(decompressInstructions di)
{
    ZSTD_freeDCtx(di.dctx);
}

/* benched function */
size_t compress(const void* src, size_t srcSize, void* dst, size_t dstCapacity, void* payload)
{
    compressInstructions* const ci = (compressInstructions*) payload;
    (void)dstCapacity;

    ZSTD_CCtx_refCDict(ci->cctx, ci->dictionaries.cdicts[ci->dictNb]);
    ZSTD_compress2(ci->cctx,
            dst, srcSize,
            src, srcSize);

    ci->dictNb = ci->dictNb + 1;
    if (ci->dictNb >= ci->nbDicts) ci->dictNb = 0;

    return srcSize;
}

/* benched function */
size_t decompress(const void* src, size_t srcSize, void* dst, size_t dstCapacity, void* payload)
{
    decompressInstructions* const di = (decompressInstructions*) payload;

    size_t const result = ZSTD_decompress_usingDDict(di->dctx,
                                        dst, dstCapacity,
                                        src, srcSize,
                                        di->dictionaries.ddicts[di->dictNb]);

    di->dictNb = di->dictNb + 1;
    if (di->dictNb >= di->nbDicts) di->dictNb = 0;

    return result;
}

typedef enum {
  fastest = 0,
  median = 1,
} metricAggregatePref_e;

/* compareFunction() :
 * Sort input in decreasing order when used with qsort() */
int compareFunction(const void *a, const void *b)
{
  double x = *(const double *)a;
  double y = *(const double *)b;
  if (x < y)
    return 1;
  else if (x > y)
    return -1;
  return 0;
}

double aggregateData(double *data, size_t size,
                     metricAggregatePref_e metricAggregatePref)
{
  qsort(data, size, sizeof(*data), compareFunction);
  if (metricAggregatePref == fastest)
    return data[0];
  else /* median */
    return (data[(size - 1) / 2] + data[size / 2]) / 2;
}

static int benchMem(slice_collection_t dstBlocks, slice_collection_t srcBlocks,
                    ddict_collection_t ddictionaries,
                    cdict_collection_t cdictionaries, unsigned nbRounds,
                    int benchCompression, const char *exeName,
                    ZSTD_CCtx_params *cctxParams,
                    metricAggregatePref_e metricAggregatePref)
{
    assert(dstBlocks.nbSlices == srcBlocks.nbSlices);
    if (benchCompression) assert(cctxParams);

    unsigned const ms_per_round = RUN_TIME_DEFAULT_MS;
    unsigned const total_time_ms = nbRounds * ms_per_round;

    double *const speedPerRound = (double *)malloc(nbRounds * sizeof(double));

    BMK_timedFnState_t* const benchState =
            BMK_createTimedFnState(total_time_ms, ms_per_round);

    decompressInstructions di = createDecompressInstructions(ddictionaries);
    compressInstructions ci =
        createCompressInstructions(cdictionaries, cctxParams);
    void* payload = benchCompression ? (void*)&ci : (void*)&di;
    BMK_benchParams_t const bp = {
        .benchFn = benchCompression ? compress : decompress,
        .benchPayload = payload,
        .initFn = NULL,
        .initPayload = NULL,
        .errorFn = ZSTD_isError,
        .blockCount = dstBlocks.nbSlices,
        .srcBuffers = (const void* const*) srcBlocks.slicePtrs,
        .srcSizes = srcBlocks.capacities,
        .dstBuffers = dstBlocks.slicePtrs,
        .dstCapacities = dstBlocks.capacities,
        .blockResults = NULL
    };

    size_t roundNb = 0;
    for (;;) {
        BMK_runOutcome_t const outcome = BMK_benchTimedFn(benchState, bp);
        CONTROL(BMK_isSuccessful_runOutcome(outcome));

        BMK_runTime_t const result = BMK_extract_runTime(outcome);
        double const dTime_ns = result.nanoSecPerRun;
        double const dTime_sec = (double)dTime_ns / 1000000000;
        size_t const srcSize = result.sumOfReturn;
        double const speed_MBps = (double)srcSize / dTime_sec / (1 MB);
        speedPerRound[roundNb] = speed_MBps;
        if (benchCompression)
            DISPLAY("Compression Speed : %.1f MB/s \r", speed_MBps);
        else
            DISPLAY("Decompression Speed : %.1f MB/s \r", speed_MBps);

        fflush(stdout);
        if (BMK_isCompleted_TimedFn(benchState)) break;
        roundNb++;
    }
    DISPLAY("\n");
    /* BMK_benchTimedFn may not run exactly nbRounds iterations */
    double speedAggregated =
        aggregateData(speedPerRound, roundNb + 1, metricAggregatePref);
    if (metricAggregatePref == fastest)
      DISPLAY("Fastest Speed : %.1f MB/s \n", speedAggregated);
    else
      DISPLAY("Median Speed : %.1f MB/s \n", speedAggregated);

    char* csvFileName = malloc(strlen(exeName) + 5);
    strcpy(csvFileName, exeName);
    strcat(csvFileName, ".csv");
    FILE* csvFile = fopen(csvFileName, "r");
    if (!csvFile) {
        csvFile = fopen(csvFileName, "wt");
        assert(csvFile);
        fprintf(csvFile, "%s\n", exeName);
        /* Print table headers */
        fprintf(
            csvFile,
            "Compression/Decompression,Level,nbDicts,dictAttachPref,metricAggregatePref,Speed\n");
    } else {
        fclose(csvFile);
        csvFile = fopen(csvFileName, "at");
        assert(csvFile);
    }

    int cLevel = -1;
    int dictAttachPref = -1;
    if (benchCompression) {
      ZSTD_CCtxParams_getParameter(cctxParams, ZSTD_c_compressionLevel,
                                   &cLevel);
      ZSTD_CCtxParams_getParameter(cctxParams, ZSTD_c_forceAttachDict,
                                   &dictAttachPref);
    }
    fprintf(csvFile, "%s,%d,%ld,%d,%d,%.1f\n",
            benchCompression ? "Compression" : "Decompression", cLevel,
            benchCompression ? ci.nbDicts : di.nbDicts, dictAttachPref,
            metricAggregatePref, speedAggregated);
    fclose(csvFile);
    free(csvFileName);

    freeDecompressInstructions(di);
    freeCompressInstructions(ci);
    BMK_freeTimedFnState(benchState);

    return 0;   /* success */
}


/*! bench() :
 *  fileName : file to load for benchmarking purpose
 *  dictionary : optional (can be NULL), file to load as dictionary,
 *              if none provided : will be calculated on the fly by the program.
 * @return : 0 is success, 1+ otherwise */
int bench(const char **fileNameTable, unsigned nbFiles, const char *dictionary,
          size_t blockSize, int clevel, unsigned nbDictMax, unsigned nbBlocks,
          unsigned nbRounds, int benchCompression,
          ZSTD_dictContentType_e dictContentType, ZSTD_CCtx_params *cctxParams,
          const char *exeName, metricAggregatePref_e metricAggregatePref)
{
    int result = 0;

    DISPLAYLEVEL(3, "loading %u files... \n", nbFiles);
    buffer_collection_t const srcs = createBufferCollection_fromFiles(fileNameTable, nbFiles);
    CONTROL(srcs.buffer.ptr != NULL);
    buffer_t srcBuffer = srcs.buffer;
    size_t const srcSize = srcBuffer.size;
    DISPLAYLEVEL(3, "created src buffer of size %.1f MB \n",
                    (double)srcSize / (1 MB));

    slice_collection_t const srcSlices = splitSlices(srcs.slices, blockSize, nbBlocks);
    nbBlocks = (unsigned)(srcSlices.nbSlices);
    DISPLAYLEVEL(3, "split input into %u blocks ", nbBlocks);
    if (blockSize)
        DISPLAYLEVEL(3, "of max size %u bytes ", (unsigned)blockSize);
    DISPLAYLEVEL(3, "\n");
    size_t const totalSrcSlicesSize = sliceCollection_totalCapacity(srcSlices);


    size_t* const dstCapacities = malloc(nbBlocks * sizeof(*dstCapacities));
    CONTROL(dstCapacities != NULL);
    size_t dstBufferCapacity = 0;
    for (size_t bnb=0; bnb<nbBlocks; bnb++) {
        dstCapacities[bnb] = ZSTD_compressBound(srcSlices.capacities[bnb]);
        dstBufferCapacity += dstCapacities[bnb];
    }

    buffer_t dstBuffer = createBuffer(dstBufferCapacity);
    CONTROL(dstBuffer.ptr != NULL);

    void** const sliceTable = malloc(nbBlocks * sizeof(*sliceTable));
    CONTROL(sliceTable != NULL);

    {   char* const ptr = dstBuffer.ptr;
        size_t pos = 0;
        for (size_t snb=0; snb < nbBlocks; snb++) {
            sliceTable[snb] = ptr + pos;
            pos += dstCapacities[snb];
    }   }

    slice_collection_t dstSlices;
    dstSlices.capacities = dstCapacities;
    dstSlices.slicePtrs = sliceTable;
    dstSlices.nbSlices = nbBlocks;


    /* dictionary determination */
    buffer_t const dictBuffer = createDictionaryBuffer(dictionary,
                                srcs.buffer.ptr,
                                srcSlices.capacities, srcSlices.nbSlices,
                                DICTSIZE);
    CONTROL(dictBuffer.ptr != NULL);

    ZSTD_CDict* const cdict = ZSTD_createCDict_advanced2(dictBuffer.ptr, dictBuffer.size, DICT_LOAD_METHOD, dictContentType, cctxParams, ZSTD_defaultCMem);
    CONTROL(cdict != NULL);

    size_t const cTotalSizeNoDict = compressBlocks(NULL, dstSlices, srcSlices, NULL, clevel);
    CONTROL(cTotalSizeNoDict != 0);
    DISPLAYLEVEL(3, "compressing at level %u without dictionary : Ratio=%.2f  (%u bytes) \n",
                    clevel,
                    (double)totalSrcSlicesSize / (double)cTotalSizeNoDict, (unsigned)cTotalSizeNoDict);

    size_t* const cSizes = malloc(nbBlocks * sizeof(size_t));
    CONTROL(cSizes != NULL);

    size_t const cTotalSize = compressBlocks(cSizes, dstSlices, srcSlices, cdict, clevel);
    CONTROL(cTotalSize != 0);
    DISPLAYLEVEL(3, "compressed using a %u bytes dictionary : Ratio=%.2f  (%u bytes) \n",
                    (unsigned)dictBuffer.size,
                    (double)totalSrcSlicesSize / (double)cTotalSize, (unsigned)cTotalSize);

    /* now dstSlices contain the real compressed size of each block, instead of the maximum capacity */
    shrinkSizes(dstSlices, cSizes);

    unsigned const nbDicts = nbDictMax ? nbDictMax : nbBlocks;

    cdict_collection_t const cdictionaries = createCDictCollection(dictBuffer.ptr, dictBuffer.size, nbDicts, dictContentType, cctxParams);
    CONTROL(cdictionaries.cdicts != NULL);

    ddict_collection_t const ddictionaries = createDDictCollection(dictBuffer.ptr, dictBuffer.size, nbDicts);
    CONTROL(ddictionaries.ddicts != NULL);

    if (benchCompression) {
        size_t const dictMem = ZSTD_sizeof_CDict(cdictionaries.cdicts[0]);
        size_t const allDictMem = dictMem * nbDicts;
        DISPLAYLEVEL(3, "generating %u dictionaries, using %.1f MB of memory \n",
                        nbDicts, (double)allDictMem / (1 MB));

        shuffleCDictionaries(cdictionaries);

        buffer_collection_t resultCollection = createBufferCollection_fromSliceCollection(srcSlices);
        CONTROL(resultCollection.buffer.ptr != NULL);

        result = benchMem(dstSlices, resultCollection.slices, ddictionaries,
                          cdictionaries, nbRounds, benchCompression, exeName,
                          cctxParams, metricAggregatePref);

        freeBufferCollection(resultCollection);
    } else {
        size_t const dictMem = ZSTD_estimateDDictSize(dictBuffer.size, DICT_LOAD_METHOD);
        size_t const allDictMem = dictMem * nbDicts;
        DISPLAYLEVEL(3, "generating %u dictionaries, using %.1f MB of memory \n",
                        nbDicts, (double)allDictMem / (1 MB));

        shuffleDDictionaries(ddictionaries);

        buffer_collection_t resultCollection = createBufferCollection_fromSliceCollectionSizes(srcSlices);
        CONTROL(resultCollection.buffer.ptr != NULL);

        result = benchMem(resultCollection.slices, dstSlices, ddictionaries,
                          cdictionaries, nbRounds, benchCompression, exeName,
                          NULL, metricAggregatePref);

        freeBufferCollection(resultCollection);
    }

    /* free all heap objects in reverse order */
    freeCDictCollection(cdictionaries);
    freeDDictCollection(ddictionaries);
    free(cSizes);
    ZSTD_freeCDict(cdict);
    freeBuffer(dictBuffer);
    freeSliceCollection(dstSlices);
    freeBuffer(dstBuffer);
    freeSliceCollection(srcSlices);
    freeBufferCollection(srcs);

    return result;
}



/* ---  Command Line  --- */

/*! readU32FromChar() :
 * @return : unsigned integer value read from input in `char` format.
 *  allows and interprets K, KB, KiB, M, MB and MiB suffix.
 *  Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 *  Note : function will exit() program if digit sequence overflows */
static unsigned readU32FromChar(const char** stringPtr)
{
    unsigned result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9')) {
        unsigned const max = (((unsigned)(-1)) / 10) - 1;
        assert(result <= max);   /* check overflow */
        result *= 10, result += (unsigned)**stringPtr - '0', (*stringPtr)++ ;
    }
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        unsigned const maxK = ((unsigned)(-1)) >> 10;
        assert(result <= maxK);   /* check overflow */
        result <<= 10;
        if (**stringPtr=='M') {
            assert(result <= maxK);   /* check overflow */
            result <<= 10;
        }
        (*stringPtr)++;  /* skip `K` or `M` */
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    return result;
}

/** longCommandWArg() :
 *  check if *stringPtr is the same as longCommand.
 *  If yes, @return 1 and advances *stringPtr to the position which immediately follows longCommand.
 * @return 0 and doesn't modify *stringPtr otherwise.
 */
static int longCommandWArg(const char** stringPtr, const char* longCommand)
{
    size_t const comSize = strlen(longCommand);
    int const result = !strncmp(*stringPtr, longCommand, comSize);
    if (result) *stringPtr += comSize;
    return result;
}


int usage(const char* exeName)
{
    DISPLAY (" \n");
    DISPLAY (" %s [Options] filename(s) \n", exeName);
    DISPLAY (" \n");
    DISPLAY ("Options : \n");
    DISPLAY ("-z          : benchmark compression (default) \n");
    DISPLAY ("-d          : benchmark decompression \n");
    DISPLAY ("-r          : recursively load all files in subdirectories (default: off) \n");
    DISPLAY ("-B#         : split input into blocks of size # (default: no split) \n");
    DISPLAY ("-#          : use compression level # (default: %u) \n", CLEVEL_DEFAULT);
    DISPLAY ("-D #        : use # as a dictionary (default: create one) \n");
    DISPLAY ("-i#         : nb benchmark rounds (default: %u) \n", BENCH_TIME_DEFAULT_S);
    DISPLAY ("-p#         : print speed for all rounds 0=fastest 1=median (default: 0) \n");
    DISPLAY ("--nbBlocks=#: use # blocks for bench (default: one per file) \n");
    DISPLAY ("--nbDicts=# : create # dictionaries for bench (default: one per block) \n");
    DISPLAY ("-h          : help (this text) \n");
    DISPLAY (" \n");
    DISPLAY ("Advanced Options (see zstd.h for documentation) : \n");
    DISPLAY ("--dedicated-dict-search\n");
    DISPLAY ("--dict-content-type=#\n");
    DISPLAY ("--dict-attach-pref=#\n");
    return 0;
}

int bad_usage(const char* exeName)
{
    DISPLAY (" bad usage : \n");
    usage(exeName);
    return 1;
}

int main (int argc, const char** argv)
{
    int recursiveMode = 0;
    int benchCompression = 1;
    int dedicatedDictSearch = 0;
    unsigned nbRounds = BENCH_TIME_DEFAULT_S;
    const char* const exeName = argv[0];

    if (argc < 2) return bad_usage(exeName);

    const char** nameTable = (const char**)malloc((size_t)argc * sizeof(const char*));
    assert(nameTable != NULL);
    unsigned nameIdx = 0;

    const char* dictionary = NULL;
    int cLevel = CLEVEL_DEFAULT;
    size_t blockSize = BLOCKSIZE_DEFAULT;
    unsigned nbDicts = 0;  /* determine nbDicts automatically: 1 dictionary per block */
    unsigned nbBlocks = 0; /* determine nbBlocks automatically, from source and blockSize */
    ZSTD_dictContentType_e dictContentType = ZSTD_dct_auto;
    ZSTD_dictAttachPref_e dictAttachPref = ZSTD_dictDefaultAttach;
    ZSTD_paramSwitch_e prefetchCDictTables = ZSTD_ps_auto;
    metricAggregatePref_e metricAggregatePref = fastest;

    for (int argNb = 1; argNb < argc ; argNb++) {
        const char* argument = argv[argNb];
        if (!strcmp(argument, "-h")) { free(nameTable); return usage(exeName); }
        if (!strcmp(argument, "-d")) { benchCompression = 0; continue; }
        if (!strcmp(argument, "-z")) { benchCompression = 1; continue; }
        if (!strcmp(argument, "-r")) { recursiveMode = 1; continue; }
        if (!strcmp(argument, "-D")) { argNb++; assert(argNb < argc); dictionary = argv[argNb]; continue; }
        if (longCommandWArg(&argument, "-i")) { nbRounds = readU32FromChar(&argument); continue; }
        if (longCommandWArg(&argument, "-p")) { metricAggregatePref = (int)readU32FromChar(&argument); continue;}
        if (longCommandWArg(&argument, "--dictionary=")) { dictionary = argument; continue; }
        if (longCommandWArg(&argument, "-B")) { blockSize = readU32FromChar(&argument); continue; }
        if (longCommandWArg(&argument, "--blockSize=")) { blockSize = readU32FromChar(&argument); continue; }
        if (longCommandWArg(&argument, "--nbDicts=")) { nbDicts = readU32FromChar(&argument); continue; }
        if (longCommandWArg(&argument, "--nbBlocks=")) { nbBlocks = readU32FromChar(&argument); continue; }
        if (longCommandWArg(&argument, "--clevel=")) { cLevel = (int)readU32FromChar(&argument); continue; }
        if (longCommandWArg(&argument, "--dedicated-dict-search")) { dedicatedDictSearch = 1; continue; }
        if (longCommandWArg(&argument, "--dict-content-type=")) { dictContentType = (int)readU32FromChar(&argument); continue; }
        if (longCommandWArg(&argument, "--dict-attach-pref=")) { dictAttachPref = (int)readU32FromChar(&argument); continue; }
        if (longCommandWArg(&argument, "--prefetch-cdict-tables=")) { prefetchCDictTables = (int)readU32FromChar(&argument); continue; }
        if (longCommandWArg(&argument, "-")) { cLevel = (int)readU32FromChar(&argument); continue; }
        /* anything that's not a command is a filename */
        nameTable[nameIdx++] = argument;
    }

    FileNamesTable* filenameTable;

    if (recursiveMode) {
#ifndef UTIL_HAS_CREATEFILELIST
        assert(0);   /* missing capability, do not run */
#endif
        filenameTable = UTIL_createExpandedFNT(nameTable, nameIdx, 1 /* follow_links */);
    } else {
        filenameTable = UTIL_assembleFileNamesTable(nameTable, nameIdx, NULL);
        nameTable = NULL;  /* UTIL_createFileNamesTable() takes ownership of nameTable */
    }

    ZSTD_CCtx_params* cctxParams = ZSTD_createCCtxParams();
    ZSTD_CCtxParams_init(cctxParams, cLevel);
    ZSTD_CCtxParams_setParameter(cctxParams, ZSTD_c_enableDedicatedDictSearch, dedicatedDictSearch);
    ZSTD_CCtxParams_setParameter(cctxParams, ZSTD_c_nbWorkers, 0);
    ZSTD_CCtxParams_setParameter(cctxParams, ZSTD_c_forceAttachDict, dictAttachPref);
    ZSTD_CCtxParams_setParameter(cctxParams, ZSTD_c_prefetchCDictTables, prefetchCDictTables);

    int result =
        bench(filenameTable->fileNames, (unsigned)filenameTable->tableSize,
              dictionary, blockSize, cLevel, nbDicts, nbBlocks, nbRounds,
              benchCompression, dictContentType, cctxParams, exeName,
              metricAggregatePref);

    UTIL_freeFileNamesTable(filenameTable);
    free(nameTable);
    ZSTD_freeCCtxParams(cctxParams);

    return result;
}
