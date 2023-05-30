/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef FILEIO_TYPES_HEADER
#define FILEIO_TYPES_HEADER

#define ZSTD_STATIC_LINKING_ONLY   /* ZSTD_compressionParameters */
#include "../lib/zstd.h"           /* ZSTD_* */

/*-*************************************
*  Parameters: FIO_prefs_t
***************************************/

typedef struct FIO_display_prefs_s FIO_display_prefs_t;

typedef enum { FIO_ps_auto, FIO_ps_never, FIO_ps_always } FIO_progressSetting_e;

struct FIO_display_prefs_s {
    int displayLevel;   /* 0 : no display;  1: errors;  2: + result + interaction + warnings;  3: + progression;  4: + information */
    FIO_progressSetting_e progressSetting;
};


typedef enum { FIO_zstdCompression, FIO_gzipCompression, FIO_xzCompression, FIO_lzmaCompression, FIO_lz4Compression } FIO_compressionType_t;

typedef struct FIO_prefs_s {

    /* Algorithm preferences */
    FIO_compressionType_t compressionType;
    int sparseFileSupport;   /* 0: no sparse allowed; 1: auto (file yes, stdout no); 2: force sparse */
    int dictIDFlag;
    int checksumFlag;
    int blockSize;
    int overlapLog;
    int adaptiveMode;
    int useRowMatchFinder;
    int rsyncable;
    int minAdaptLevel;
    int maxAdaptLevel;
    int ldmFlag;
    int ldmHashLog;
    int ldmMinMatch;
    int ldmBucketSizeLog;
    int ldmHashRateLog;
    size_t streamSrcSize;
    size_t targetCBlockSize;
    int srcSizeHint;
    int testMode;
    ZSTD_paramSwitch_e literalCompressionMode;

    /* IO preferences */
    int removeSrcFile;
    int overwrite;
    int asyncIO;

    /* Computation resources preferences */
    unsigned memLimit;
    int nbWorkers;

    int excludeCompressedFiles;
    int patchFromMode;
    int contentSize;
    int allowBlockDevices;
    int passThrough;
    ZSTD_paramSwitch_e mmapDict;
} FIO_prefs_t;

typedef enum {FIO_mallocDict, FIO_mmapDict} FIO_dictBufferType_t;

typedef struct {
    void* dictBuffer;
    size_t dictBufferSize;
    FIO_dictBufferType_t dictBufferType;
#if defined(_MSC_VER) || defined(_WIN32)
    HANDLE dictHandle;
#endif
} FIO_Dict_t;

#endif /* FILEIO_TYPES_HEADER */
