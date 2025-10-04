/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */



/* **************************************
*  Compiler Warnings
****************************************/
#ifdef _MSC_VER
#  pragma warning(disable : 4127)    /* disable: C4127: conditional expression is constant */
#endif


/*-*************************************
*  Includes
***************************************/
#include "platform.h"       /* Large Files support */
#include "util.h"           /* UTIL_getFileSize, UTIL_getTotalFileSize */
#include <stdlib.h>         /* malloc, free */
#include <string.h>         /* memset */
#include <stdio.h>          /* fprintf, fopen, ftello64 */
#include <errno.h>          /* errno */

#include "timefn.h"         /* UTIL_time_t, UTIL_clockSpanMicro, UTIL_getTime */
#include "../lib/common/debug.h" /* assert */
#include "../lib/common/mem.h"  /* read */
#include "../lib/zstd_errors.h"
#include "dibio.h"


/*-*************************************
*  Constants
***************************************/
#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)

#define SAMPLESIZE_MAX (128 KB)
#define MEMMULT 11    /* rough estimation : memory cost to analyze 1 byte of sample */
#define COVER_MEMMULT 9    /* rough estimation : memory cost to analyze 1 byte of sample */
#define FASTCOVER_MEMMULT 1    /* rough estimation : memory cost to analyze 1 byte of sample */
static const size_t g_maxMemory = (sizeof(size_t) == 4) ? (2 GB - 64 MB) : ((size_t)(512 MB) << sizeof(size_t));

#define NOISELENGTH 32
#define MAX_SAMPLES_SIZE (2 GB) /* training dataset limited to 2GB */


/*-*************************************
*  Console display
***************************************/
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (displayLevel>=l) { DISPLAY(__VA_ARGS__); }

static const U64 g_refreshRate = SEC_TO_MICRO / 6;
static UTIL_time_t g_displayClock = UTIL_TIME_INITIALIZER;

#define DISPLAYUPDATE(l, ...) { if (displayLevel>=l) { \
            if ((UTIL_clockSpanMicro(g_displayClock) > g_refreshRate) || (displayLevel>=4)) \
            { g_displayClock = UTIL_getTime(); DISPLAY(__VA_ARGS__); \
            if (displayLevel>=4) fflush(stderr); } } }

/*-*************************************
*  Exceptions
***************************************/
#ifndef DEBUG
#  define DEBUG 0
#endif
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAY("Error %i : ", error);                                        \
    DISPLAY(__VA_ARGS__);                                                 \
    DISPLAY("\n");                                                        \
    exit(error);                                                          \
}


/* ********************************************************
*  Helper functions
**********************************************************/
#undef MIN
#define MIN(a,b)    ((a) < (b) ? (a) : (b))

/**
  Returns the size of a file.
  If error returns -1.
*/
static S64 DiB_getFileSize (const char * fileName)
{
    U64 const fileSize = UTIL_getFileSize(fileName);
    return (fileSize == UTIL_FILESIZE_UNKNOWN) ? -1 : (S64)fileSize;
}

/* ********************************************************
*  File related operations
**********************************************************/
/** DiB_loadFiles() :
 *  load samples from files listed in fileNamesTable into buffer.
 *  works even if buffer is too small to load all samples.
 *  Also provides the size of each sample into sampleSizes table
 *  which must be sized correctly, using DiB_fileStats().
 * @return : nb of samples effectively loaded into `buffer`
 * *bufferSizePtr is modified, it provides the amount data loaded within buffer.
 *  sampleSizes is filled with the size of each sample.
 */
static int DiB_loadFiles(
    void* buffer, size_t* bufferSizePtr,
    size_t* sampleSizes, int sstSize,
    const char** fileNamesTable, int nbFiles,
    size_t targetChunkSize, int displayLevel )
{
    char* const buff = (char*)buffer;
    size_t totalDataLoaded = 0;
    int nbSamplesLoaded = 0;
    int fileIndex = 0;
    FILE * f = NULL;

    assert(targetChunkSize <= SAMPLESIZE_MAX);

    while ( nbSamplesLoaded < sstSize && fileIndex < nbFiles ) {
        size_t fileDataLoaded;
        S64 const fileSize = DiB_getFileSize(fileNamesTable[fileIndex]);
        if (fileSize <= 0) {
            /* skip if zero-size or file error */
            ++fileIndex;
            continue;
        }

        f = fopen( fileNamesTable[fileIndex], "rb");
        if (f == NULL)
            EXM_THROW(10, "zstd: dictBuilder: %s %s ", fileNamesTable[fileIndex], strerror(errno));
        DISPLAYUPDATE(2, "Loading %s...       \r", fileNamesTable[fileIndex]);

        /* Load the first chunk of data from the file */
        fileDataLoaded = targetChunkSize > 0 ?
                            (size_t)MIN(fileSize, (S64)targetChunkSize) :
                            (size_t)MIN(fileSize, SAMPLESIZE_MAX );
        if (totalDataLoaded + fileDataLoaded > *bufferSizePtr)
            break;
        if (fread( buff+totalDataLoaded, 1, fileDataLoaded, f ) != fileDataLoaded)
            EXM_THROW(11, "Pb reading %s", fileNamesTable[fileIndex]);
        sampleSizes[nbSamplesLoaded++] = fileDataLoaded;
        totalDataLoaded += fileDataLoaded;

        /* If file-chunking is enabled, load the rest of the file as more samples */
        if (targetChunkSize > 0) {
            while( (S64)fileDataLoaded < fileSize && nbSamplesLoaded < sstSize ) {
                size_t const chunkSize = MIN((size_t)(fileSize-fileDataLoaded), targetChunkSize);
                if (totalDataLoaded + chunkSize > *bufferSizePtr) /* buffer is full */
                    break;

                if (fread( buff+totalDataLoaded, 1, chunkSize, f ) != chunkSize)
                    EXM_THROW(11, "Pb reading %s", fileNamesTable[fileIndex]);
                sampleSizes[nbSamplesLoaded++] = chunkSize;
                totalDataLoaded += chunkSize;
                fileDataLoaded += chunkSize;
            }
        }
        fileIndex += 1;
        fclose(f); f = NULL;
    }
    if (f != NULL)
        fclose(f);

    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(4, "Loaded %d KB total training data, %d nb samples \n",
        (int)(totalDataLoaded / (1 KB)), nbSamplesLoaded );
    *bufferSizePtr = totalDataLoaded;
    return nbSamplesLoaded;
}

#define DiB_rotl32(x,r) ((x << r) | (x >> (32 - r)))
static U32 DiB_rand(U32* src)
{
    static const U32 prime1 = 2654435761U;
    static const U32 prime2 = 2246822519U;
    U32 rand32 = *src;
    rand32 *= prime1;
    rand32 ^= prime2;
    rand32  = DiB_rotl32(rand32, 13);
    *src = rand32;
    return rand32 >> 5;
}

/* DiB_shuffle() :
 * shuffle a table of file names in a semi-random way
 * It improves dictionary quality by reducing "locality" impact, so if sample set is very large,
 * it will load random elements from it, instead of just the first ones. */
static void DiB_shuffle(const char** fileNamesTable, unsigned nbFiles) {
    U32 seed = 0xFD2FB528;
    unsigned i;
    if (nbFiles == 0)
        return;
    for (i = nbFiles - 1; i > 0; --i) {
        unsigned const j = DiB_rand(&seed) % (i + 1);
        const char* const tmp = fileNamesTable[j];
        fileNamesTable[j] = fileNamesTable[i];
        fileNamesTable[i] = tmp;
    }
}


/*-********************************************************
*  Dictionary training functions
**********************************************************/
static size_t DiB_findMaxMem(unsigned long long requiredMem)
{
    size_t const step = 8 MB;
    void* testmem = NULL;

    requiredMem = (((requiredMem >> 23) + 1) << 23);
    requiredMem += step;
    if (requiredMem > g_maxMemory) requiredMem = g_maxMemory;

    while (!testmem) {
        testmem = malloc((size_t)requiredMem);
        requiredMem -= step;
    }

    free(testmem);
    return (size_t)requiredMem;
}


static void DiB_fillNoise(void* buffer, size_t length)
{
    unsigned const prime1 = 2654435761U;
    unsigned const prime2 = 2246822519U;
    unsigned acc = prime1;
    size_t p=0;

    for (p=0; p<length; p++) {
        acc *= prime2;
        ((unsigned char*)buffer)[p] = (unsigned char)(acc >> 21);
    }
}


static void DiB_saveDict(const char* dictFileName,
                         const void* buff, size_t buffSize)
{
    FILE* const f = fopen(dictFileName, "wb");
    if (f==NULL) EXM_THROW(3, "cannot open %s ", dictFileName);

    { size_t const n = fwrite(buff, 1, buffSize, f);
      if (n!=buffSize) EXM_THROW(4, "%s : write error", dictFileName) }

    { size_t const n = (size_t)fclose(f);
      if (n!=0) EXM_THROW(5, "%s : flush error", dictFileName) }
}

typedef struct {
    S64 totalSizeToLoad;
    int nbSamples;
    int oneSampleTooLarge;
} fileStats;

/*! DiB_fileStats() :
 *  Given a list of files, and a chunkSize (0 == no chunk, whole files)
 *  provides the amount of data to be loaded and the resulting nb of samples.
 *  This is useful primarily for allocation purpose => sample buffer, and sample sizes table.
 */
static fileStats DiB_fileStats(const char** fileNamesTable, int nbFiles, size_t chunkSize, int displayLevel)
{
    fileStats fs;
    int n;
    memset(&fs, 0, sizeof(fs));

    /* We assume that if chunking is requested, the chunk size is < SAMPLESIZE_MAX */
    assert( chunkSize <= SAMPLESIZE_MAX );

    for (n=0; n<nbFiles; n++) {
      S64 const fileSize = DiB_getFileSize(fileNamesTable[n]);
      /* TODO: is there a minimum sample size? What if the file is 1-byte? */
      if (fileSize == 0) {
        DISPLAYLEVEL(3, "Sample file '%s' has zero size, skipping...\n", fileNamesTable[n]);
        continue;
      }

      /* the case where we are breaking up files in sample chunks */
      if (chunkSize > 0) {
        /* TODO: is there a minimum sample size? Can we have a 1-byte sample? */
        fs.nbSamples += (int)((fileSize + chunkSize-1) / chunkSize);
        fs.totalSizeToLoad += fileSize;
      }
      else {
      /* the case where one file is one sample */
        if (fileSize > SAMPLESIZE_MAX) {
          /* flag excessively large sample files */
          fs.oneSampleTooLarge |= (fileSize > 2*SAMPLESIZE_MAX);

          /* Limit to the first SAMPLESIZE_MAX (128kB) of the file */
          DISPLAYLEVEL(3, "Sample file '%s' is too large, limiting to %d KB",
              fileNamesTable[n], SAMPLESIZE_MAX / (1 KB));
        }
        fs.nbSamples += 1;
        fs.totalSizeToLoad += MIN(fileSize, SAMPLESIZE_MAX);
      }
    }
    DISPLAYLEVEL(4, "Found training data %d files, %d KB, %d samples\n", nbFiles, (int)(fs.totalSizeToLoad / (1 KB)), fs.nbSamples);
    return fs;
}

int DiB_trainFromFiles(const char* dictFileName, size_t maxDictSize,
                       const char** fileNamesTable, int nbFiles, size_t chunkSize,
                       ZDICT_legacy_params_t* params, ZDICT_cover_params_t* coverParams,
                       ZDICT_fastCover_params_t* fastCoverParams, int optimize, unsigned memLimit)
{
    fileStats fs;
    size_t* sampleSizes; /* vector of sample sizes. Each sample can be up to SAMPLESIZE_MAX */
    int nbSamplesLoaded; /* nb of samples effectively loaded in srcBuffer */
    size_t loadedSize; /* total data loaded in srcBuffer for all samples */
    void* srcBuffer /* contiguous buffer with training data/samples */;
    void* const dictBuffer = malloc(maxDictSize);
    int result = 0;

    int const displayLevel = params ? params->zParams.notificationLevel :
        coverParams ? coverParams->zParams.notificationLevel :
        fastCoverParams ? fastCoverParams->zParams.notificationLevel : 0;

    /* Shuffle input files before we start assessing how much sample datA to load.
       The purpose of the shuffle is to pick random samples when the sample
       set is larger than what we can load in memory. */
    DISPLAYLEVEL(3, "Shuffling input files\n");
    DiB_shuffle(fileNamesTable, nbFiles);

    /* Figure out how much sample data to load with how many samples */
    fs = DiB_fileStats(fileNamesTable, nbFiles, chunkSize, displayLevel);

    {
        int const memMult = params ? MEMMULT :
                            coverParams ? COVER_MEMMULT:
                            FASTCOVER_MEMMULT;
        size_t const maxMem =  DiB_findMaxMem(fs.totalSizeToLoad * memMult) / memMult;
        /* Limit the size of the training data to the free memory */
        /* Limit the size of the training data to 2GB */
        /* TODO: there is opportunity to stop DiB_fileStats() early when the data limit is reached */
        loadedSize = (size_t)MIN( MIN((S64)maxMem, fs.totalSizeToLoad), MAX_SAMPLES_SIZE );
        if (memLimit != 0) {
            DISPLAYLEVEL(2, "!  Warning : setting manual memory limit for dictionary training data at %u MB \n",
                (unsigned)(memLimit / (1 MB)));
            loadedSize = (size_t)MIN(loadedSize, memLimit);
        }
        srcBuffer = malloc(loadedSize+NOISELENGTH);
        sampleSizes = (size_t*)malloc(fs.nbSamples * sizeof(size_t));
    }

    /* Checks */
    if ((fs.nbSamples && !sampleSizes) || (!srcBuffer) || (!dictBuffer))
        EXM_THROW(12, "not enough memory for DiB_trainFiles");   /* should not happen */
    if (fs.oneSampleTooLarge) {
        DISPLAYLEVEL(2, "!  Warning : some sample(s) are very large \n");
        DISPLAYLEVEL(2, "!  Note that dictionary is only useful for small samples. \n");
        DISPLAYLEVEL(2, "!  As a consequence, only the first %u bytes of each sample are loaded \n", SAMPLESIZE_MAX);
    }
    if (fs.nbSamples < 5) {
        DISPLAYLEVEL(2, "!  Warning : nb of samples too low for proper processing ! \n");
        DISPLAYLEVEL(2, "!  Please provide _one file per sample_. \n");
        DISPLAYLEVEL(2, "!  Alternatively, split files into fixed-size blocks representative of samples, with -B# \n");
        EXM_THROW(14, "nb of samples too low");   /* we now clearly forbid this case */
    }
    if (fs.totalSizeToLoad < (S64)maxDictSize * 8) {
        DISPLAYLEVEL(2, "!  Warning : data size of samples too small for target dictionary size \n");
        DISPLAYLEVEL(2, "!  Samples should be about 100x larger than target dictionary size \n");
    }

    /* init */
    if ((S64)loadedSize < fs.totalSizeToLoad)
        DISPLAYLEVEL(1, "Training samples set too large (%u MB); training on %u MB only...\n",
            (unsigned)(fs.totalSizeToLoad / (1 MB)),
            (unsigned)(loadedSize / (1 MB)));

    /* Load input buffer */
    nbSamplesLoaded = DiB_loadFiles(
        srcBuffer, &loadedSize, sampleSizes, fs.nbSamples, fileNamesTable,
        nbFiles, chunkSize, displayLevel);

    {   size_t dictSize = ZSTD_error_GENERIC;
        if (params) {
            DiB_fillNoise((char*)srcBuffer + loadedSize, NOISELENGTH);   /* guard band, for end of buffer condition */
            dictSize = ZDICT_trainFromBuffer_legacy(dictBuffer, maxDictSize,
                                                    srcBuffer, sampleSizes, nbSamplesLoaded,
                                                    *params);
        } else if (coverParams) {
            if (optimize) {
              dictSize = ZDICT_optimizeTrainFromBuffer_cover(dictBuffer, maxDictSize,
                                                             srcBuffer, sampleSizes, nbSamplesLoaded,
                                                             coverParams);
              if (!ZDICT_isError(dictSize)) {
                  unsigned splitPercentage = (unsigned)(coverParams->splitPoint * 100);
                  DISPLAYLEVEL(2, "k=%u\nd=%u\nsteps=%u\nsplit=%u\n", coverParams->k, coverParams->d,
                              coverParams->steps, splitPercentage);
              }
            } else {
              dictSize = ZDICT_trainFromBuffer_cover(dictBuffer, maxDictSize, srcBuffer,
                                                     sampleSizes, nbSamplesLoaded, *coverParams);
            }
        } else if (fastCoverParams != NULL) {
            if (optimize) {
              dictSize = ZDICT_optimizeTrainFromBuffer_fastCover(dictBuffer, maxDictSize,
                                                              srcBuffer, sampleSizes, nbSamplesLoaded,
                                                              fastCoverParams);
              if (!ZDICT_isError(dictSize)) {
                unsigned splitPercentage = (unsigned)(fastCoverParams->splitPoint * 100);
                DISPLAYLEVEL(2, "k=%u\nd=%u\nf=%u\nsteps=%u\nsplit=%u\naccel=%u\n", fastCoverParams->k,
                            fastCoverParams->d, fastCoverParams->f, fastCoverParams->steps, splitPercentage,
                            fastCoverParams->accel);
              }
            } else {
              dictSize = ZDICT_trainFromBuffer_fastCover(dictBuffer, maxDictSize, srcBuffer,
                                                        sampleSizes, nbSamplesLoaded, *fastCoverParams);
            }
        } else {
            assert(0 /* Impossible */);
        }
        if (ZDICT_isError(dictSize)) {
            DISPLAYLEVEL(1, "dictionary training failed : %s \n", ZDICT_getErrorName(dictSize));   /* should not happen */
            result = 1;
            goto _cleanup;
        }
        /* save dict */
        DISPLAYLEVEL(2, "Save dictionary of size %u into file %s \n", (unsigned)dictSize, dictFileName);
        DiB_saveDict(dictFileName, dictBuffer, dictSize);
    }

    /* clean up */
_cleanup:
    free(srcBuffer);
    free(sampleSizes);
    free(dictBuffer);
    return result;
}
