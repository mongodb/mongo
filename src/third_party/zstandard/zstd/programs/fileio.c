/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


/* *************************************
*  Compiler Options
***************************************/
#ifdef _MSC_VER   /* Visual */
#  pragma warning(disable : 4127)  /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4204)  /* non-constant aggregate initializer */
#endif
#if defined(__MINGW32__) && !defined(_POSIX_SOURCE)
#  define _POSIX_SOURCE 1          /* disable %llu warnings with MinGW on Windows */
#endif

/*-*************************************
*  Includes
***************************************/
#include "platform.h"   /* Large Files support, SET_BINARY_MODE */
#include "util.h"       /* UTIL_getFileSize, UTIL_isRegularFile, UTIL_isSameFile */
#include <stdio.h>      /* fprintf, open, fdopen, fread, _fileno, stdin, stdout */
#include <stdlib.h>     /* malloc, free */
#include <string.h>     /* strcmp, strlen */
#include <time.h>       /* clock_t, to measure process time */
#include <fcntl.h>      /* O_WRONLY */
#include <assert.h>
#include <errno.h>      /* errno */
#include <limits.h>     /* INT_MAX */
#include <signal.h>
#include "timefn.h"     /* UTIL_getTime, UTIL_clockSpanMicro */

#if defined (_MSC_VER)
#  include <sys/stat.h>
#  include <io.h>
#endif

#include "fileio.h"
#include "fileio_asyncio.h"
#include "fileio_common.h"

FIO_display_prefs_t g_display_prefs = {2, FIO_ps_auto};
UTIL_time_t g_displayClock = UTIL_TIME_INITIALIZER;

#define ZSTD_STATIC_LINKING_ONLY   /* ZSTD_magicNumber, ZSTD_frameHeaderSize_max */
#include "../lib/zstd.h"
#include "../lib/zstd_errors.h"  /* ZSTD_error_frameParameter_windowTooLarge */

#if defined(ZSTD_GZCOMPRESS) || defined(ZSTD_GZDECOMPRESS)
#  include <zlib.h>
#  if !defined(z_const)
#    define z_const
#  endif
#endif

#if defined(ZSTD_LZMACOMPRESS) || defined(ZSTD_LZMADECOMPRESS)
#  include <lzma.h>
#endif

#define LZ4_MAGICNUMBER 0x184D2204
#if defined(ZSTD_LZ4COMPRESS) || defined(ZSTD_LZ4DECOMPRESS)
#  define LZ4F_ENABLE_OBSOLETE_ENUMS
#  include <lz4frame.h>
#  include <lz4.h>
#endif

char const* FIO_zlibVersion(void)
{
#if defined(ZSTD_GZCOMPRESS) || defined(ZSTD_GZDECOMPRESS)
    return zlibVersion();
#else
    return "Unsupported";
#endif
}

char const* FIO_lz4Version(void)
{
#if defined(ZSTD_LZ4COMPRESS) || defined(ZSTD_LZ4DECOMPRESS)
    /* LZ4_versionString() added in v1.7.3 */
#   if LZ4_VERSION_NUMBER >= 10703
        return LZ4_versionString();
#   else
#       define ZSTD_LZ4_VERSION LZ4_VERSION_MAJOR.LZ4_VERSION_MINOR.LZ4_VERSION_RELEASE
#       define ZSTD_LZ4_VERSION_STRING ZSTD_EXPAND_AND_QUOTE(ZSTD_LZ4_VERSION)
        return ZSTD_LZ4_VERSION_STRING;
#   endif
#else
    return "Unsupported";
#endif
}

char const* FIO_lzmaVersion(void)
{
#if defined(ZSTD_LZMACOMPRESS) || defined(ZSTD_LZMADECOMPRESS)
    return lzma_version_string();
#else
    return "Unsupported";
#endif
}


/*-*************************************
*  Constants
***************************************/
#define ADAPT_WINDOWLOG_DEFAULT 23   /* 8 MB */
#define DICTSIZE_MAX (32 MB)   /* protection against large input (attack scenario) */

#define FNSPACE 30

/* Default file permissions 0666 (modulated by umask) */
/* Temporary restricted file permissions are used when we're going to
 * chmod/chown at the end of the operation. */
#if !defined(_WIN32)
/* These macros aren't defined on windows. */
#define DEFAULT_FILE_PERMISSIONS (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH)
#define TEMPORARY_FILE_PERMISSIONS (S_IRUSR|S_IWUSR)
#else
#define DEFAULT_FILE_PERMISSIONS (0666)
#define TEMPORARY_FILE_PERMISSIONS (0600)
#endif

/*-************************************
*  Signal (Ctrl-C trapping)
**************************************/
static const char* g_artefact = NULL;
static void INThandler(int sig)
{
    assert(sig==SIGINT); (void)sig;
#if !defined(_MSC_VER)
    signal(sig, SIG_IGN);  /* this invocation generates a buggy warning in Visual Studio */
#endif
    if (g_artefact) {
        assert(UTIL_isRegularFile(g_artefact));
        remove(g_artefact);
    }
    DISPLAY("\n");
    exit(2);
}
static void addHandler(char const* dstFileName)
{
    if (UTIL_isRegularFile(dstFileName)) {
        g_artefact = dstFileName;
        signal(SIGINT, INThandler);
    } else {
        g_artefact = NULL;
    }
}
/* Idempotent */
static void clearHandler(void)
{
    if (g_artefact) signal(SIGINT, SIG_DFL);
    g_artefact = NULL;
}


/*-*********************************************************
*  Termination signal trapping (Print debug stack trace)
***********************************************************/
#if defined(__has_feature) && !defined(BACKTRACE_ENABLE) /* Clang compiler */
#  if (__has_feature(address_sanitizer))
#    define BACKTRACE_ENABLE 0
#  endif /* __has_feature(address_sanitizer) */
#elif defined(__SANITIZE_ADDRESS__) && !defined(BACKTRACE_ENABLE) /* GCC compiler */
#  define BACKTRACE_ENABLE 0
#endif

#if !defined(BACKTRACE_ENABLE)
/* automatic detector : backtrace enabled by default on linux+glibc and osx */
#  if (defined(__linux__) && (defined(__GLIBC__) && !defined(__UCLIBC__))) \
     || (defined(__APPLE__) && defined(__MACH__))
#    define BACKTRACE_ENABLE 1
#  else
#    define BACKTRACE_ENABLE 0
#  endif
#endif

/* note : after this point, BACKTRACE_ENABLE is necessarily defined */


#if BACKTRACE_ENABLE

#include <execinfo.h>   /* backtrace, backtrace_symbols */

#define MAX_STACK_FRAMES    50

static void ABRThandler(int sig) {
    const char* name;
    void* addrlist[MAX_STACK_FRAMES];
    char** symbollist;
    int addrlen, i;

    switch (sig) {
        case SIGABRT: name = "SIGABRT"; break;
        case SIGFPE: name = "SIGFPE"; break;
        case SIGILL: name = "SIGILL"; break;
        case SIGINT: name = "SIGINT"; break;
        case SIGSEGV: name = "SIGSEGV"; break;
        default: name = "UNKNOWN";
    }

    DISPLAY("Caught %s signal, printing stack:\n", name);
    /* Retrieve current stack addresses. */
    addrlen = backtrace(addrlist, MAX_STACK_FRAMES);
    if (addrlen == 0) {
        DISPLAY("\n");
        return;
    }
    /* Create readable strings to each frame. */
    symbollist = backtrace_symbols(addrlist, addrlen);
    /* Print the stack trace, excluding calls handling the signal. */
    for (i = ZSTD_START_SYMBOLLIST_FRAME; i < addrlen; i++) {
        DISPLAY("%s\n", symbollist[i]);
    }
    free(symbollist);
    /* Reset and raise the signal so default handler runs. */
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

void FIO_addAbortHandler(void)
{
#if BACKTRACE_ENABLE
    signal(SIGABRT, ABRThandler);
    signal(SIGFPE, ABRThandler);
    signal(SIGILL, ABRThandler);
    signal(SIGSEGV, ABRThandler);
    signal(SIGBUS, ABRThandler);
#endif
}

/*-*************************************
*  Parameters: FIO_ctx_t
***************************************/

/* typedef'd to FIO_ctx_t within fileio.h */
struct FIO_ctx_s {

    /* file i/o info */
    int nbFilesTotal;
    int hasStdinInput;
    int hasStdoutOutput;

    /* file i/o state */
    int currFileIdx;
    int nbFilesProcessed;
    size_t totalBytesInput;
    size_t totalBytesOutput;
};

static int FIO_shouldDisplayFileSummary(FIO_ctx_t const* fCtx)
{
    return fCtx->nbFilesTotal <= 1 || g_display_prefs.displayLevel >= 3;
}

static int FIO_shouldDisplayMultipleFileSummary(FIO_ctx_t const* fCtx)
{
    int const shouldDisplay = (fCtx->nbFilesProcessed >= 1 && fCtx->nbFilesTotal > 1);
    assert(shouldDisplay || FIO_shouldDisplayFileSummary(fCtx) || fCtx->nbFilesProcessed == 0);
    return shouldDisplay;
}


/*-*************************************
*  Parameters: Initialization
***************************************/

#define FIO_OVERLAP_LOG_NOTSET 9999
#define FIO_LDM_PARAM_NOTSET 9999


FIO_prefs_t* FIO_createPreferences(void)
{
    FIO_prefs_t* const ret = (FIO_prefs_t*)malloc(sizeof(FIO_prefs_t));
    if (!ret) EXM_THROW(21, "Allocation error : not enough memory");

    ret->compressionType = FIO_zstdCompression;
    ret->overwrite = 0;
    ret->sparseFileSupport = ZSTD_SPARSE_DEFAULT;
    ret->dictIDFlag = 1;
    ret->checksumFlag = 1;
    ret->removeSrcFile = 0;
    ret->memLimit = 0;
    ret->nbWorkers = 1;
    ret->blockSize = 0;
    ret->overlapLog = FIO_OVERLAP_LOG_NOTSET;
    ret->adaptiveMode = 0;
    ret->rsyncable = 0;
    ret->minAdaptLevel = -50;   /* initializing this value requires a constant, so ZSTD_minCLevel() doesn't work */
    ret->maxAdaptLevel = 22;   /* initializing this value requires a constant, so ZSTD_maxCLevel() doesn't work */
    ret->ldmFlag = 0;
    ret->ldmHashLog = 0;
    ret->ldmMinMatch = 0;
    ret->ldmBucketSizeLog = FIO_LDM_PARAM_NOTSET;
    ret->ldmHashRateLog = FIO_LDM_PARAM_NOTSET;
    ret->streamSrcSize = 0;
    ret->targetCBlockSize = 0;
    ret->srcSizeHint = 0;
    ret->testMode = 0;
    ret->literalCompressionMode = ZSTD_ps_auto;
    ret->excludeCompressedFiles = 0;
    ret->allowBlockDevices = 0;
    ret->asyncIO = AIO_supported();
    ret->passThrough = -1;
    return ret;
}

FIO_ctx_t* FIO_createContext(void)
{
    FIO_ctx_t* const ret = (FIO_ctx_t*)malloc(sizeof(FIO_ctx_t));
    if (!ret) EXM_THROW(21, "Allocation error : not enough memory");

    ret->currFileIdx = 0;
    ret->hasStdinInput = 0;
    ret->hasStdoutOutput = 0;
    ret->nbFilesTotal = 1;
    ret->nbFilesProcessed = 0;
    ret->totalBytesInput = 0;
    ret->totalBytesOutput = 0;
    return ret;
}

void FIO_freePreferences(FIO_prefs_t* const prefs)
{
    free(prefs);
}

void FIO_freeContext(FIO_ctx_t* const fCtx)
{
    free(fCtx);
}


/*-*************************************
*  Parameters: Display Options
***************************************/

void FIO_setNotificationLevel(int level) { g_display_prefs.displayLevel=level; }

void FIO_setProgressSetting(FIO_progressSetting_e setting) { g_display_prefs.progressSetting = setting; }


/*-*************************************
*  Parameters: Setters
***************************************/

/* FIO_prefs_t functions */

void FIO_setCompressionType(FIO_prefs_t* const prefs, FIO_compressionType_t compressionType) { prefs->compressionType = compressionType; }

void FIO_overwriteMode(FIO_prefs_t* const prefs) { prefs->overwrite = 1; }

void FIO_setSparseWrite(FIO_prefs_t* const prefs, int sparse) { prefs->sparseFileSupport = sparse; }

void FIO_setDictIDFlag(FIO_prefs_t* const prefs, int dictIDFlag) { prefs->dictIDFlag = dictIDFlag; }

void FIO_setChecksumFlag(FIO_prefs_t* const prefs, int checksumFlag) { prefs->checksumFlag = checksumFlag; }

void FIO_setRemoveSrcFile(FIO_prefs_t* const prefs, int flag) { prefs->removeSrcFile = (flag!=0); }

void FIO_setMemLimit(FIO_prefs_t* const prefs, unsigned memLimit) { prefs->memLimit = memLimit; }

void FIO_setNbWorkers(FIO_prefs_t* const prefs, int nbWorkers) {
#ifndef ZSTD_MULTITHREAD
    if (nbWorkers > 0) DISPLAYLEVEL(2, "Note : multi-threading is disabled \n");
#endif
    prefs->nbWorkers = nbWorkers;
}

void FIO_setExcludeCompressedFile(FIO_prefs_t* const prefs, int excludeCompressedFiles) { prefs->excludeCompressedFiles = excludeCompressedFiles; }

void FIO_setAllowBlockDevices(FIO_prefs_t* const prefs, int allowBlockDevices) { prefs->allowBlockDevices = allowBlockDevices; }

void FIO_setBlockSize(FIO_prefs_t* const prefs, int blockSize) {
    if (blockSize && prefs->nbWorkers==0)
        DISPLAYLEVEL(2, "Setting block size is useless in single-thread mode \n");
    prefs->blockSize = blockSize;
}

void FIO_setOverlapLog(FIO_prefs_t* const prefs, int overlapLog){
    if (overlapLog && prefs->nbWorkers==0)
        DISPLAYLEVEL(2, "Setting overlapLog is useless in single-thread mode \n");
    prefs->overlapLog = overlapLog;
}

void FIO_setAdaptiveMode(FIO_prefs_t* const prefs, int adapt) {
    if ((adapt>0) && (prefs->nbWorkers==0))
        EXM_THROW(1, "Adaptive mode is not compatible with single thread mode \n");
    prefs->adaptiveMode = adapt;
}

void FIO_setUseRowMatchFinder(FIO_prefs_t* const prefs, int useRowMatchFinder) {
    prefs->useRowMatchFinder = useRowMatchFinder;
}

void FIO_setRsyncable(FIO_prefs_t* const prefs, int rsyncable) {
    if ((rsyncable>0) && (prefs->nbWorkers==0))
        EXM_THROW(1, "Rsyncable mode is not compatible with single thread mode \n");
    prefs->rsyncable = rsyncable;
}

void FIO_setStreamSrcSize(FIO_prefs_t* const prefs, size_t streamSrcSize) {
    prefs->streamSrcSize = streamSrcSize;
}

void FIO_setTargetCBlockSize(FIO_prefs_t* const prefs, size_t targetCBlockSize) {
    prefs->targetCBlockSize = targetCBlockSize;
}

void FIO_setSrcSizeHint(FIO_prefs_t* const prefs, size_t srcSizeHint) {
    prefs->srcSizeHint = (int)MIN((size_t)INT_MAX, srcSizeHint);
}

void FIO_setTestMode(FIO_prefs_t* const prefs, int testMode) {
    prefs->testMode = (testMode!=0);
}

void FIO_setLiteralCompressionMode(
        FIO_prefs_t* const prefs,
        ZSTD_paramSwitch_e mode) {
    prefs->literalCompressionMode = mode;
}

void FIO_setAdaptMin(FIO_prefs_t* const prefs, int minCLevel)
{
#ifndef ZSTD_NOCOMPRESS
    assert(minCLevel >= ZSTD_minCLevel());
#endif
    prefs->minAdaptLevel = minCLevel;
}

void FIO_setAdaptMax(FIO_prefs_t* const prefs, int maxCLevel)
{
    prefs->maxAdaptLevel = maxCLevel;
}

void FIO_setLdmFlag(FIO_prefs_t* const prefs, unsigned ldmFlag) {
    prefs->ldmFlag = (ldmFlag>0);
}

void FIO_setLdmHashLog(FIO_prefs_t* const prefs, int ldmHashLog) {
    prefs->ldmHashLog = ldmHashLog;
}

void FIO_setLdmMinMatch(FIO_prefs_t* const prefs, int ldmMinMatch) {
    prefs->ldmMinMatch = ldmMinMatch;
}

void FIO_setLdmBucketSizeLog(FIO_prefs_t* const prefs, int ldmBucketSizeLog) {
    prefs->ldmBucketSizeLog = ldmBucketSizeLog;
}


void FIO_setLdmHashRateLog(FIO_prefs_t* const prefs, int ldmHashRateLog) {
    prefs->ldmHashRateLog = ldmHashRateLog;
}

void FIO_setPatchFromMode(FIO_prefs_t* const prefs, int value)
{
    prefs->patchFromMode = value != 0;
}

void FIO_setContentSize(FIO_prefs_t* const prefs, int value)
{
    prefs->contentSize = value != 0;
}

void FIO_setAsyncIOFlag(FIO_prefs_t* const prefs, int value) {
#ifdef ZSTD_MULTITHREAD
    prefs->asyncIO = value;
#else
    (void) prefs;
    (void) value;
    DISPLAYLEVEL(2, "Note : asyncio is disabled (lack of multithreading support) \n");
#endif
}

void FIO_setPassThroughFlag(FIO_prefs_t* const prefs, int value) {
    prefs->passThrough = (value != 0);
}

void FIO_setMMapDict(FIO_prefs_t* const prefs, ZSTD_paramSwitch_e value)
{
    prefs->mmapDict = value;
}

/* FIO_ctx_t functions */

void FIO_setHasStdoutOutput(FIO_ctx_t* const fCtx, int value) {
    fCtx->hasStdoutOutput = value;
}

void FIO_setNbFilesTotal(FIO_ctx_t* const fCtx, int value)
{
    fCtx->nbFilesTotal = value;
}

void FIO_determineHasStdinInput(FIO_ctx_t* const fCtx, const FileNamesTable* const filenames) {
    size_t i = 0;
    for ( ; i < filenames->tableSize; ++i) {
        if (!strcmp(stdinmark, filenames->fileNames[i])) {
            fCtx->hasStdinInput = 1;
            return;
        }
    }
}

/*-*************************************
*  Functions
***************************************/
/** FIO_removeFile() :
 * @result : Unlink `fileName`, even if it's read-only */
static int FIO_removeFile(const char* path)
{
    stat_t statbuf;
    if (!UTIL_stat(path, &statbuf)) {
        DISPLAYLEVEL(2, "zstd: Failed to stat %s while trying to remove it\n", path);
        return 0;
    }
    if (!UTIL_isRegularFileStat(&statbuf)) {
        DISPLAYLEVEL(2, "zstd: Refusing to remove non-regular file %s\n", path);
        return 0;
    }
#if defined(_WIN32) || defined(WIN32)
    /* windows doesn't allow remove read-only files,
     * so try to make it writable first */
    if (!(statbuf.st_mode & _S_IWRITE)) {
        UTIL_chmod(path, &statbuf, _S_IWRITE);
    }
#endif
    return remove(path);
}

/** FIO_openSrcFile() :
 *  condition : `srcFileName` must be non-NULL. `prefs` may be NULL.
 * @result : FILE* to `srcFileName`, or NULL if it fails */
static FILE* FIO_openSrcFile(const FIO_prefs_t* const prefs, const char* srcFileName, stat_t* statbuf)
{
    int allowBlockDevices = prefs != NULL ? prefs->allowBlockDevices : 0;
    assert(srcFileName != NULL);
    assert(statbuf != NULL);
    if (!strcmp (srcFileName, stdinmark)) {
        DISPLAYLEVEL(4,"Using stdin for input \n");
        SET_BINARY_MODE(stdin);
        return stdin;
    }

    if (!UTIL_stat(srcFileName, statbuf)) {
        DISPLAYLEVEL(1, "zstd: can't stat %s : %s -- ignored \n",
                        srcFileName, strerror(errno));
        return NULL;
    }

    if (!UTIL_isRegularFileStat(statbuf)
     && !UTIL_isFIFOStat(statbuf)
     && !(allowBlockDevices && UTIL_isBlockDevStat(statbuf))
    ) {
        DISPLAYLEVEL(1, "zstd: %s is not a regular file -- ignored \n",
                        srcFileName);
        return NULL;
    }

    {   FILE* const f = fopen(srcFileName, "rb");
        if (f == NULL)
            DISPLAYLEVEL(1, "zstd: %s: %s \n", srcFileName, strerror(errno));
        return f;
    }
}

/** FIO_openDstFile() :
 *  condition : `dstFileName` must be non-NULL.
 * @result : FILE* to `dstFileName`, or NULL if it fails */
static FILE*
FIO_openDstFile(FIO_ctx_t* fCtx, FIO_prefs_t* const prefs,
                const char* srcFileName, const char* dstFileName,
                const int mode)
{
    int isDstRegFile;

    if (prefs->testMode) return NULL;  /* do not open file in test mode */

    assert(dstFileName != NULL);
    if (!strcmp (dstFileName, stdoutmark)) {
        DISPLAYLEVEL(4,"Using stdout for output \n");
        SET_BINARY_MODE(stdout);
        if (prefs->sparseFileSupport == 1) {
            prefs->sparseFileSupport = 0;
            DISPLAYLEVEL(4, "Sparse File Support is automatically disabled on stdout ; try --sparse \n");
        }
        return stdout;
    }

    /* ensure dst is not the same as src */
    if (srcFileName != NULL && UTIL_isSameFile(srcFileName, dstFileName)) {
        DISPLAYLEVEL(1, "zstd: Refusing to open an output file which will overwrite the input file \n");
        return NULL;
    }

    isDstRegFile = UTIL_isRegularFile(dstFileName);  /* invoke once */
    if (prefs->sparseFileSupport == 1) {
        prefs->sparseFileSupport = ZSTD_SPARSE_DEFAULT;
        if (!isDstRegFile) {
            prefs->sparseFileSupport = 0;
            DISPLAYLEVEL(4, "Sparse File Support is disabled when output is not a file \n");
        }
    }

    if (isDstRegFile) {
        /* Check if destination file already exists */
#if !defined(_WIN32)
        /* this test does not work on Windows :
         * `NUL` and `nul` are detected as regular files */
        if (!strcmp(dstFileName, nulmark)) {
            EXM_THROW(40, "%s is unexpectedly categorized as a regular file",
                        dstFileName);
        }
#endif
        if (!prefs->overwrite) {
            if (g_display_prefs.displayLevel <= 1) {
                /* No interaction possible */
                DISPLAYLEVEL(1, "zstd: %s already exists; not overwritten  \n",
                        dstFileName);
                return NULL;
            }
            DISPLAY("zstd: %s already exists; ", dstFileName);
            if (UTIL_requireUserConfirmation("overwrite (y/n) ? ", "Not overwritten  \n", "yY", fCtx->hasStdinInput))
                return NULL;
        }
        /* need to unlink */
        FIO_removeFile(dstFileName);
    }

    {
#if defined(_WIN32)
        /* Windows requires opening the file as a "binary" file to avoid
         * mangling. This macro doesn't exist on unix. */
        const int openflags = O_WRONLY|O_CREAT|O_TRUNC|O_BINARY;
        const int fd = _open(dstFileName, openflags, mode);
        FILE* f = NULL;
        if (fd != -1) {
            f = _fdopen(fd, "wb");
        }
#else
        const int openflags = O_WRONLY|O_CREAT|O_TRUNC;
        const int fd = open(dstFileName, openflags, mode);
        FILE* f = NULL;
        if (fd != -1) {
            f = fdopen(fd, "wb");
        }
#endif
        if (f == NULL) {
            DISPLAYLEVEL(1, "zstd: %s: %s\n", dstFileName, strerror(errno));
        } else {
            /* An increased buffer size can provide a significant performance
             * boost on some platforms. Note that providing a NULL buf with a
             * size that's not 0 is not defined in ANSI C, but is defined in an
             * extension. There are three possibilities here:
             * 1. Libc supports the extended version and everything is good.
             * 2. Libc ignores the size when buf is NULL, in which case
             *    everything will continue as if we didn't call `setvbuf()`.
             * 3. We fail the call and execution continues but a warning
             *    message might be shown.
             * In all cases due execution continues. For now, I believe that
             * this is a more cost-effective solution than managing the buffers
             * allocations ourselves (will require an API change).
             */
            if (setvbuf(f, NULL, _IOFBF, 1 MB)) {
                DISPLAYLEVEL(2, "Warning: setvbuf failed for %s\n", dstFileName);
            }
        }
        return f;
    }
}


/* FIO_getDictFileStat() :
 */
static void FIO_getDictFileStat(const char* fileName, stat_t* dictFileStat) {
    assert(dictFileStat != NULL);
    if (fileName == NULL) return;

    if (!UTIL_stat(fileName, dictFileStat)) {
        EXM_THROW(31, "Stat failed on dictionary file %s: %s", fileName, strerror(errno));
    }

    if (!UTIL_isRegularFileStat(dictFileStat)) {
        EXM_THROW(32, "Dictionary %s must be a regular file.", fileName);
    }
}

/*  FIO_setDictBufferMalloc() :
 *  allocates a buffer, pointed by `dict->dictBuffer`,
 *  loads `filename` content into it, up to DICTSIZE_MAX bytes.
 * @return : loaded size
 *  if fileName==NULL, returns 0 and a NULL pointer
 */
static size_t FIO_setDictBufferMalloc(FIO_Dict_t* dict, const char* fileName, FIO_prefs_t* const prefs, stat_t* dictFileStat)
{
    FILE* fileHandle;
    U64 fileSize;
    void** bufferPtr = &dict->dictBuffer;

    assert(bufferPtr != NULL);
    assert(dictFileStat != NULL);
    *bufferPtr = NULL;
    if (fileName == NULL) return 0;

    DISPLAYLEVEL(4,"Loading %s as dictionary \n", fileName);

    fileHandle = fopen(fileName, "rb");

    if (fileHandle == NULL) {
        EXM_THROW(33, "Couldn't open dictionary %s: %s", fileName, strerror(errno));
    }

    fileSize = UTIL_getFileSizeStat(dictFileStat);
    {
        size_t const dictSizeMax = prefs->patchFromMode ? prefs->memLimit : DICTSIZE_MAX;
        if (fileSize >  dictSizeMax) {
            EXM_THROW(34, "Dictionary file %s is too large (> %u bytes)",
                            fileName,  (unsigned)dictSizeMax);   /* avoid extreme cases */
        }
    }
    *bufferPtr = malloc((size_t)fileSize);
    if (*bufferPtr==NULL) EXM_THROW(34, "%s", strerror(errno));
    {   size_t const readSize = fread(*bufferPtr, 1, (size_t)fileSize, fileHandle);
        if (readSize != fileSize) {
            EXM_THROW(35, "Error reading dictionary file %s : %s",
                    fileName, strerror(errno));
        }
    }
    fclose(fileHandle);
    return (size_t)fileSize;
}

#if (PLATFORM_POSIX_VERSION > 0)
#include <sys/mman.h>
static void FIO_munmap(FIO_Dict_t* dict)
{
    munmap(dict->dictBuffer, dict->dictBufferSize);
    dict->dictBuffer = NULL;
    dict->dictBufferSize = 0;
}
static size_t FIO_setDictBufferMMap(FIO_Dict_t* dict, const char* fileName, FIO_prefs_t* const prefs, stat_t* dictFileStat)
{
    int fileHandle;
    U64 fileSize;
    void** bufferPtr = &dict->dictBuffer;

    assert(bufferPtr != NULL);
    assert(dictFileStat != NULL);
    *bufferPtr = NULL;
    if (fileName == NULL) return 0;

    DISPLAYLEVEL(4,"Loading %s as dictionary \n", fileName);

    fileHandle = open(fileName, O_RDONLY);

    if (fileHandle == -1) {
        EXM_THROW(33, "Couldn't open dictionary %s: %s", fileName, strerror(errno));
    }

    fileSize = UTIL_getFileSizeStat(dictFileStat);
    {
        size_t const dictSizeMax = prefs->patchFromMode ? prefs->memLimit : DICTSIZE_MAX;
        if (fileSize >  dictSizeMax) {
            EXM_THROW(34, "Dictionary file %s is too large (> %u bytes)",
                            fileName,  (unsigned)dictSizeMax);   /* avoid extreme cases */
        }
    }

    *bufferPtr = mmap(NULL, (size_t)fileSize, PROT_READ, MAP_PRIVATE, fileHandle, 0);
    if (*bufferPtr==NULL) EXM_THROW(34, "%s", strerror(errno));

    close(fileHandle);
    return (size_t)fileSize;
}
#elif defined(_MSC_VER) || defined(_WIN32)
#include <windows.h>
static void FIO_munmap(FIO_Dict_t* dict)
{
    UnmapViewOfFile(dict->dictBuffer);
    CloseHandle(dict->dictHandle);
    dict->dictBuffer = NULL;
    dict->dictBufferSize = 0;
}
static size_t FIO_setDictBufferMMap(FIO_Dict_t* dict, const char* fileName, FIO_prefs_t* const prefs, stat_t* dictFileStat)
{
    HANDLE fileHandle, mapping;
    U64 fileSize;
    void** bufferPtr = &dict->dictBuffer;

    assert(bufferPtr != NULL);
    assert(dictFileStat != NULL);
    *bufferPtr = NULL;
    if (fileName == NULL) return 0;

    DISPLAYLEVEL(4,"Loading %s as dictionary \n", fileName);

    fileHandle = CreateFileA(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);

    if (fileHandle == INVALID_HANDLE_VALUE) {
        EXM_THROW(33, "Couldn't open dictionary %s: %s", fileName, strerror(errno));
    }

    fileSize = UTIL_getFileSizeStat(dictFileStat);
    {
        size_t const dictSizeMax = prefs->patchFromMode ? prefs->memLimit : DICTSIZE_MAX;
        if (fileSize >  dictSizeMax) {
            EXM_THROW(34, "Dictionary file %s is too large (> %u bytes)",
                            fileName,  (unsigned)dictSizeMax);   /* avoid extreme cases */
        }
    }

    mapping = CreateFileMapping(fileHandle, NULL, PAGE_READONLY, 0, 0, NULL);
	if (mapping == NULL) {
        EXM_THROW(35, "Couldn't map dictionary %s: %s", fileName, strerror(errno));
    }

	*bufferPtr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, (DWORD)fileSize); /* we can only cast to DWORD here because dictSize <= 2GB */
	if (*bufferPtr==NULL) EXM_THROW(36, "%s", strerror(errno));

    dict->dictHandle = fileHandle;
    return (size_t)fileSize;
}
#else
static size_t FIO_setDictBufferMMap(FIO_Dict_t* dict, const char* fileName, FIO_prefs_t* const prefs, stat_t* dictFileStat)
{
   return FIO_setDictBufferMalloc(dict, fileName, prefs, dictFileStat);
}
static void FIO_munmap(FIO_Dict_t* dict) {
   free(dict->dictBuffer);
   dict->dictBuffer = NULL;
   dict->dictBufferSize = 0;
}
#endif

static void FIO_freeDict(FIO_Dict_t* dict) {
    if (dict->dictBufferType == FIO_mallocDict) {
        free(dict->dictBuffer);
        dict->dictBuffer = NULL;
        dict->dictBufferSize = 0;
    } else if (dict->dictBufferType == FIO_mmapDict)  {
        FIO_munmap(dict);
    } else {
        assert(0); /* Should not reach this case */
    }
}

static void FIO_initDict(FIO_Dict_t* dict, const char* fileName, FIO_prefs_t* const prefs, stat_t* dictFileStat, FIO_dictBufferType_t dictBufferType) {
    dict->dictBufferType = dictBufferType;
    if (dict->dictBufferType == FIO_mallocDict) {
        dict->dictBufferSize = FIO_setDictBufferMalloc(dict, fileName, prefs, dictFileStat);
    } else if (dict->dictBufferType == FIO_mmapDict)  {
        dict->dictBufferSize = FIO_setDictBufferMMap(dict, fileName, prefs, dictFileStat);
    } else {
        assert(0); /* Should not reach this case */
    }
}


/* FIO_checkFilenameCollisions() :
 * Checks for and warns if there are any files that would have the same output path
 */
int FIO_checkFilenameCollisions(const char** filenameTable, unsigned nbFiles) {
    const char **filenameTableSorted, *prevElem, *filename;
    unsigned u;

    filenameTableSorted = (const char**) malloc(sizeof(char*) * nbFiles);
    if (!filenameTableSorted) {
        DISPLAYLEVEL(1, "Allocation error during filename collision checking \n");
        return 1;
    }

    for (u = 0; u < nbFiles; ++u) {
        filename = strrchr(filenameTable[u], PATH_SEP);
        if (filename == NULL) {
            filenameTableSorted[u] = filenameTable[u];
        } else {
            filenameTableSorted[u] = filename+1;
        }
    }

    qsort((void*)filenameTableSorted, nbFiles, sizeof(char*), UTIL_compareStr);
    prevElem = filenameTableSorted[0];
    for (u = 1; u < nbFiles; ++u) {
        if (strcmp(prevElem, filenameTableSorted[u]) == 0) {
            DISPLAYLEVEL(2, "WARNING: Two files have same filename: %s\n", prevElem);
        }
        prevElem = filenameTableSorted[u];
    }

    free((void*)filenameTableSorted);
    return 0;
}

static const char*
extractFilename(const char* path, char separator)
{
    const char* search = strrchr(path, separator);
    if (search == NULL) return path;
    return search+1;
}

/* FIO_createFilename_fromOutDir() :
 * Takes a source file name and specified output directory, and
 * allocates memory for and returns a pointer to final path.
 * This function never returns an error (it may abort() in case of pb)
 */
static char*
FIO_createFilename_fromOutDir(const char* path, const char* outDirName, const size_t suffixLen)
{
    const char* filenameStart;
    char separator;
    char* result;

#if defined(_MSC_VER) || defined(__MINGW32__) || defined (__MSVCRT__) /* windows support */
    separator = '\\';
#else
    separator = '/';
#endif

    filenameStart = extractFilename(path, separator);
#if defined(_MSC_VER) || defined(__MINGW32__) || defined (__MSVCRT__) /* windows support */
    filenameStart = extractFilename(filenameStart, '/');  /* sometimes, '/' separator is also used on Windows (mingw+msys2) */
#endif

    result = (char*) calloc(1, strlen(outDirName) + 1 + strlen(filenameStart) + suffixLen + 1);
    if (!result) {
        EXM_THROW(30, "zstd: FIO_createFilename_fromOutDir: %s", strerror(errno));
    }

    memcpy(result, outDirName, strlen(outDirName));
    if (outDirName[strlen(outDirName)-1] == separator) {
        memcpy(result + strlen(outDirName), filenameStart, strlen(filenameStart));
    } else {
        memcpy(result + strlen(outDirName), &separator, 1);
        memcpy(result + strlen(outDirName) + 1, filenameStart, strlen(filenameStart));
    }

    return result;
}

/* FIO_highbit64() :
 * gives position of highest bit.
 * note : only works for v > 0 !
 */
static unsigned FIO_highbit64(unsigned long long v)
{
    unsigned count = 0;
    assert(v != 0);
    v >>= 1;
    while (v) { v >>= 1; count++; }
    return count;
}

static void FIO_adjustMemLimitForPatchFromMode(FIO_prefs_t* const prefs,
                                    unsigned long long const dictSize,
                                    unsigned long long const maxSrcFileSize)
{
    unsigned long long maxSize = MAX(prefs->memLimit, MAX(dictSize, maxSrcFileSize));
    unsigned const maxWindowSize = (1U << ZSTD_WINDOWLOG_MAX);
    if (maxSize == UTIL_FILESIZE_UNKNOWN)
        EXM_THROW(42, "Using --patch-from with stdin requires --stream-size");
    assert(maxSize != UTIL_FILESIZE_UNKNOWN);
    if (maxSize > maxWindowSize)
        EXM_THROW(42, "Can't handle files larger than %u GB\n", maxWindowSize/(1 GB));
    FIO_setMemLimit(prefs, (unsigned)maxSize);
}

/* FIO_multiFilesConcatWarning() :
 * This function handles logic when processing multiple files with -o or -c, displaying the appropriate warnings/prompts.
 * Returns 1 if the console should abort, 0 if console should proceed.
 *
 * If output is stdout or test mode is active, check that `--rm` disabled.
 *
 * If there is just 1 file to process, zstd will proceed as usual.
 * If each file get processed into its own separate destination file, proceed as usual.
 *
 * When multiple files are processed into a single output,
 * display a warning message, then disable --rm if it's set.
 *
 * If -f is specified or if output is stdout, just proceed.
 * If output is set with -o, prompt for confirmation.
 */
static int FIO_multiFilesConcatWarning(const FIO_ctx_t* fCtx, FIO_prefs_t* prefs, const char* outFileName, int displayLevelCutoff)
{
    if (fCtx->hasStdoutOutput) {
        if (prefs->removeSrcFile)
            /* this should not happen ; hard fail, to protect user's data
             * note: this should rather be an assert(), but we want to be certain that user's data will not be wiped out in case it nonetheless happen */
            EXM_THROW(43, "It's not allowed to remove input files when processed output is piped to stdout. "
                "This scenario is not supposed to be possible. "
                "This is a programming error. File an issue for it to be fixed.");
    }
    if (prefs->testMode) {
        if (prefs->removeSrcFile)
            /* this should not happen ; hard fail, to protect user's data
             * note: this should rather be an assert(), but we want to be certain that user's data will not be wiped out in case it nonetheless happen */
            EXM_THROW(43, "Test mode shall not remove input files! "
                 "This scenario is not supposed to be possible. "
                 "This is a programming error. File an issue for it to be fixed.");
        return 0;
    }

    if (fCtx->nbFilesTotal == 1) return 0;
    assert(fCtx->nbFilesTotal > 1);

    if (!outFileName) return 0;

    if (fCtx->hasStdoutOutput) {
        DISPLAYLEVEL(2, "zstd: WARNING: all input files will be processed and concatenated into stdout. \n");
    } else {
        DISPLAYLEVEL(2, "zstd: WARNING: all input files will be processed and concatenated into a single output file: %s \n", outFileName);
    }
    DISPLAYLEVEL(2, "The concatenated output CANNOT regenerate original file names nor directory structure. \n")

    /* multi-input into single output : --rm is not allowed */
    if (prefs->removeSrcFile) {
        DISPLAYLEVEL(2, "Since it's a destructive operation, input files will not be removed. \n");
        prefs->removeSrcFile = 0;
    }

    if (fCtx->hasStdoutOutput) return 0;
    if (prefs->overwrite) return 0;

    /* multiple files concatenated into single destination file using -o without -f */
    if (g_display_prefs.displayLevel <= displayLevelCutoff) {
        /* quiet mode => no prompt => fail automatically */
        DISPLAYLEVEL(1, "Concatenating multiple processed inputs into a single output loses file metadata. \n");
        DISPLAYLEVEL(1, "Aborting. \n");
        return 1;
    }
    /* normal mode => prompt */
    return UTIL_requireUserConfirmation("Proceed? (y/n): ", "Aborting...", "yY", fCtx->hasStdinInput);
}

static ZSTD_inBuffer setInBuffer(const void* buf, size_t s, size_t pos)
{
    ZSTD_inBuffer i;
    i.src = buf;
    i.size = s;
    i.pos = pos;
    return i;
}

static ZSTD_outBuffer setOutBuffer(void* buf, size_t s, size_t pos)
{
    ZSTD_outBuffer o;
    o.dst = buf;
    o.size = s;
    o.pos = pos;
    return o;
}

#ifndef ZSTD_NOCOMPRESS

/* **********************************************************************
 *  Compression
 ************************************************************************/
typedef struct {
    FIO_Dict_t dict;
    const char* dictFileName;
    stat_t dictFileStat;
    ZSTD_CStream* cctx;
    WritePoolCtx_t *writeCtx;
    ReadPoolCtx_t *readCtx;
} cRess_t;

/** ZSTD_cycleLog() :
 *  condition for correct operation : hashLog > 1 */
static U32 ZSTD_cycleLog(U32 hashLog, ZSTD_strategy strat)
{
    U32 const btScale = ((U32)strat >= (U32)ZSTD_btlazy2);
    assert(hashLog > 1);
    return hashLog - btScale;
}

static void FIO_adjustParamsForPatchFromMode(FIO_prefs_t* const prefs,
                                    ZSTD_compressionParameters* comprParams,
                                    unsigned long long const dictSize,
                                    unsigned long long const maxSrcFileSize,
                                    int cLevel)
{
    unsigned const fileWindowLog = FIO_highbit64(maxSrcFileSize) + 1;
    ZSTD_compressionParameters const cParams = ZSTD_getCParams(cLevel, (size_t)maxSrcFileSize, (size_t)dictSize);
    FIO_adjustMemLimitForPatchFromMode(prefs, dictSize, maxSrcFileSize);
    if (fileWindowLog > ZSTD_WINDOWLOG_MAX)
        DISPLAYLEVEL(1, "Max window log exceeded by file (compression ratio will suffer)\n");
    comprParams->windowLog = MAX(ZSTD_WINDOWLOG_MIN, MIN(ZSTD_WINDOWLOG_MAX, fileWindowLog));
    if (fileWindowLog > ZSTD_cycleLog(cParams.chainLog, cParams.strategy)) {
        if (!prefs->ldmFlag)
            DISPLAYLEVEL(1, "long mode automatically triggered\n");
        FIO_setLdmFlag(prefs, 1);
    }
    if (cParams.strategy >= ZSTD_btopt) {
        DISPLAYLEVEL(1, "[Optimal parser notes] Consider the following to improve patch size at the cost of speed:\n");
        DISPLAYLEVEL(1, "- Use --single-thread mode in the zstd cli\n");
        DISPLAYLEVEL(1, "- Set a larger targetLength (e.g. --zstd=targetLength=4096)\n");
        DISPLAYLEVEL(1, "- Set a larger chainLog (e.g. --zstd=chainLog=%u)\n", ZSTD_CHAINLOG_MAX);
        DISPLAYLEVEL(1, "Also consider playing around with searchLog and hashLog\n");
    }
}

static cRess_t FIO_createCResources(FIO_prefs_t* const prefs,
                                    const char* dictFileName, unsigned long long const maxSrcFileSize,
                                    int cLevel, ZSTD_compressionParameters comprParams) {
    int useMMap = prefs->mmapDict == ZSTD_ps_enable;
    int forceNoUseMMap = prefs->mmapDict == ZSTD_ps_disable;
    FIO_dictBufferType_t dictBufferType;
    cRess_t ress;
    memset(&ress, 0, sizeof(ress));

    DISPLAYLEVEL(6, "FIO_createCResources \n");
    ress.cctx = ZSTD_createCCtx();
    if (ress.cctx == NULL)
        EXM_THROW(30, "allocation error (%s): can't create ZSTD_CCtx",
                    strerror(errno));

    FIO_getDictFileStat(dictFileName, &ress.dictFileStat);

    /* need to update memLimit before calling createDictBuffer
     * because of memLimit check inside it */
    if (prefs->patchFromMode) {
        U64 const dictSize = UTIL_getFileSizeStat(&ress.dictFileStat);
        unsigned long long const ssSize = (unsigned long long)prefs->streamSrcSize;
        useMMap |= dictSize > prefs->memLimit;
        FIO_adjustParamsForPatchFromMode(prefs, &comprParams, dictSize, ssSize > 0 ? ssSize : maxSrcFileSize, cLevel);
    }

    dictBufferType = (useMMap && !forceNoUseMMap) ? FIO_mmapDict : FIO_mallocDict;
    FIO_initDict(&ress.dict, dictFileName, prefs, &ress.dictFileStat, dictBufferType);   /* works with dictFileName==NULL */

    ress.writeCtx = AIO_WritePool_create(prefs, ZSTD_CStreamOutSize());
    ress.readCtx = AIO_ReadPool_create(prefs, ZSTD_CStreamInSize());

    /* Advanced parameters, including dictionary */
    if (dictFileName && (ress.dict.dictBuffer==NULL))
        EXM_THROW(32, "allocation error : can't create dictBuffer");
    ress.dictFileName = dictFileName;

    if (prefs->adaptiveMode && !prefs->ldmFlag && !comprParams.windowLog)
        comprParams.windowLog = ADAPT_WINDOWLOG_DEFAULT;

    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_contentSizeFlag, prefs->contentSize) );  /* always enable content size when available (note: supposed to be default) */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_dictIDFlag, prefs->dictIDFlag) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_checksumFlag, prefs->checksumFlag) );
    /* compression level */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_compressionLevel, cLevel) );
    /* max compressed block size */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_targetCBlockSize, (int)prefs->targetCBlockSize) );
    /* source size hint */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_srcSizeHint, (int)prefs->srcSizeHint) );
    /* long distance matching */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_enableLongDistanceMatching, prefs->ldmFlag) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_ldmHashLog, prefs->ldmHashLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_ldmMinMatch, prefs->ldmMinMatch) );
    if (prefs->ldmBucketSizeLog != FIO_LDM_PARAM_NOTSET) {
        CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_ldmBucketSizeLog, prefs->ldmBucketSizeLog) );
    }
    if (prefs->ldmHashRateLog != FIO_LDM_PARAM_NOTSET) {
        CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_ldmHashRateLog, prefs->ldmHashRateLog) );
    }
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_useRowMatchFinder, prefs->useRowMatchFinder));
    /* compression parameters */
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_windowLog, (int)comprParams.windowLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_chainLog, (int)comprParams.chainLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_hashLog, (int)comprParams.hashLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_searchLog, (int)comprParams.searchLog) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_minMatch, (int)comprParams.minMatch) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_targetLength, (int)comprParams.targetLength) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_strategy, (int)comprParams.strategy) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_literalCompressionMode, (int)prefs->literalCompressionMode) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_enableDedicatedDictSearch, 1) );
    /* multi-threading */
#ifdef ZSTD_MULTITHREAD
    DISPLAYLEVEL(5,"set nb workers = %u \n", prefs->nbWorkers);
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_nbWorkers, prefs->nbWorkers) );
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_jobSize, prefs->blockSize) );
    if (prefs->overlapLog != FIO_OVERLAP_LOG_NOTSET) {
        DISPLAYLEVEL(3,"set overlapLog = %u \n", prefs->overlapLog);
        CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_overlapLog, prefs->overlapLog) );
    }
    CHECK( ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_rsyncable, prefs->rsyncable) );
#endif
    /* dictionary */
    if (prefs->patchFromMode) {
        CHECK( ZSTD_CCtx_refPrefix(ress.cctx, ress.dict.dictBuffer, ress.dict.dictBufferSize) );
    } else {
        CHECK( ZSTD_CCtx_loadDictionary_byReference(ress.cctx, ress.dict.dictBuffer, ress.dict.dictBufferSize) );
    }

    return ress;
}

static void FIO_freeCResources(cRess_t* const ress)
{
    FIO_freeDict(&(ress->dict));
    AIO_WritePool_free(ress->writeCtx);
    AIO_ReadPool_free(ress->readCtx);
    ZSTD_freeCStream(ress->cctx);   /* never fails */
}


#ifdef ZSTD_GZCOMPRESS
static unsigned long long
FIO_compressGzFrame(const cRess_t* ress,  /* buffers & handlers are used, but not changed */
                    const char* srcFileName, U64 const srcFileSize,
                    int compressionLevel, U64* readsize)
{
    unsigned long long inFileSize = 0, outFileSize = 0;
    z_stream strm;
    IOJob_t *writeJob = NULL;

    if (compressionLevel > Z_BEST_COMPRESSION)
        compressionLevel = Z_BEST_COMPRESSION;

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;

    {   int const ret = deflateInit2(&strm, compressionLevel, Z_DEFLATED,
                        15 /* maxWindowLogSize */ + 16 /* gzip only */,
                        8, Z_DEFAULT_STRATEGY); /* see https://www.zlib.net/manual.html */
        if (ret != Z_OK) {
            EXM_THROW(71, "zstd: %s: deflateInit2 error %d \n", srcFileName, ret);
    }   }

    writeJob = AIO_WritePool_acquireJob(ress->writeCtx);
    strm.next_in = 0;
    strm.avail_in = 0;
    strm.next_out = (Bytef*)writeJob->buffer;
    strm.avail_out = (uInt)writeJob->bufferSize;

    while (1) {
        int ret;
        if (strm.avail_in == 0) {
            AIO_ReadPool_fillBuffer(ress->readCtx, ZSTD_CStreamInSize());
            if (ress->readCtx->srcBufferLoaded == 0) break;
            inFileSize += ress->readCtx->srcBufferLoaded;
            strm.next_in = (z_const unsigned char*)ress->readCtx->srcBuffer;
            strm.avail_in = (uInt)ress->readCtx->srcBufferLoaded;
        }

        {
            size_t const availBefore = strm.avail_in;
            ret = deflate(&strm, Z_NO_FLUSH);
            AIO_ReadPool_consumeBytes(ress->readCtx, availBefore - strm.avail_in);
        }

        if (ret != Z_OK)
            EXM_THROW(72, "zstd: %s: deflate error %d \n", srcFileName, ret);
        {   size_t const cSize = writeJob->bufferSize - strm.avail_out;
            if (cSize) {
                writeJob->usedBufferSize = cSize;
                AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
                outFileSize += cSize;
                strm.next_out = (Bytef*)writeJob->buffer;
                strm.avail_out = (uInt)writeJob->bufferSize;
            }   }
        if (srcFileSize == UTIL_FILESIZE_UNKNOWN) {
            DISPLAYUPDATE_PROGRESS(
                    "\rRead : %u MB ==> %.2f%% ",
                    (unsigned)(inFileSize>>20),
                    (double)outFileSize/(double)inFileSize*100)
        } else {
            DISPLAYUPDATE_PROGRESS(
                    "\rRead : %u / %u MB ==> %.2f%% ",
                    (unsigned)(inFileSize>>20), (unsigned)(srcFileSize>>20),
                    (double)outFileSize/(double)inFileSize*100);
    }   }

    while (1) {
        int const ret = deflate(&strm, Z_FINISH);
        {   size_t const cSize = writeJob->bufferSize - strm.avail_out;
            if (cSize) {
                writeJob->usedBufferSize = cSize;
                AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
                outFileSize += cSize;
                strm.next_out = (Bytef*)writeJob->buffer;
                strm.avail_out = (uInt)writeJob->bufferSize;
            }   }
        if (ret == Z_STREAM_END) break;
        if (ret != Z_BUF_ERROR)
            EXM_THROW(77, "zstd: %s: deflate error %d \n", srcFileName, ret);
    }

    {   int const ret = deflateEnd(&strm);
        if (ret != Z_OK) {
            EXM_THROW(79, "zstd: %s: deflateEnd error %d \n", srcFileName, ret);
    }   }
    *readsize = inFileSize;
    AIO_WritePool_releaseIoJob(writeJob);
    AIO_WritePool_sparseWriteEnd(ress->writeCtx);
    return outFileSize;
}
#endif


#ifdef ZSTD_LZMACOMPRESS
static unsigned long long
FIO_compressLzmaFrame(cRess_t* ress,
                      const char* srcFileName, U64 const srcFileSize,
                      int compressionLevel, U64* readsize, int plain_lzma)
{
    unsigned long long inFileSize = 0, outFileSize = 0;
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_action action = LZMA_RUN;
    lzma_ret ret;
    IOJob_t *writeJob = NULL;

    if (compressionLevel < 0) compressionLevel = 0;
    if (compressionLevel > 9) compressionLevel = 9;

    if (plain_lzma) {
        lzma_options_lzma opt_lzma;
        if (lzma_lzma_preset(&opt_lzma, compressionLevel))
            EXM_THROW(81, "zstd: %s: lzma_lzma_preset error", srcFileName);
        ret = lzma_alone_encoder(&strm, &opt_lzma); /* LZMA */
        if (ret != LZMA_OK)
            EXM_THROW(82, "zstd: %s: lzma_alone_encoder error %d", srcFileName, ret);
    } else {
        ret = lzma_easy_encoder(&strm, compressionLevel, LZMA_CHECK_CRC64); /* XZ */
        if (ret != LZMA_OK)
            EXM_THROW(83, "zstd: %s: lzma_easy_encoder error %d", srcFileName, ret);
    }

    writeJob =AIO_WritePool_acquireJob(ress->writeCtx);
    strm.next_out = (BYTE*)writeJob->buffer;
    strm.avail_out = writeJob->bufferSize;
    strm.next_in = 0;
    strm.avail_in = 0;

    while (1) {
        if (strm.avail_in == 0) {
            size_t const inSize = AIO_ReadPool_fillBuffer(ress->readCtx, ZSTD_CStreamInSize());
            if (ress->readCtx->srcBufferLoaded == 0) action = LZMA_FINISH;
            inFileSize += inSize;
            strm.next_in = (BYTE const*)ress->readCtx->srcBuffer;
            strm.avail_in = ress->readCtx->srcBufferLoaded;
        }

        {
            size_t const availBefore = strm.avail_in;
            ret = lzma_code(&strm, action);
            AIO_ReadPool_consumeBytes(ress->readCtx, availBefore - strm.avail_in);
        }


        if (ret != LZMA_OK && ret != LZMA_STREAM_END)
            EXM_THROW(84, "zstd: %s: lzma_code encoding error %d", srcFileName, ret);
        {   size_t const compBytes = writeJob->bufferSize - strm.avail_out;
            if (compBytes) {
                writeJob->usedBufferSize = compBytes;
                AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
                outFileSize += compBytes;
                strm.next_out = (BYTE*)writeJob->buffer;
                strm.avail_out = writeJob->bufferSize;
        }   }
        if (srcFileSize == UTIL_FILESIZE_UNKNOWN)
            DISPLAYUPDATE_PROGRESS("\rRead : %u MB ==> %.2f%%",
                            (unsigned)(inFileSize>>20),
                            (double)outFileSize/(double)inFileSize*100)
        else
            DISPLAYUPDATE_PROGRESS("\rRead : %u / %u MB ==> %.2f%%",
                            (unsigned)(inFileSize>>20), (unsigned)(srcFileSize>>20),
                            (double)outFileSize/(double)inFileSize*100);
        if (ret == LZMA_STREAM_END) break;
    }

    lzma_end(&strm);
    *readsize = inFileSize;

    AIO_WritePool_releaseIoJob(writeJob);
    AIO_WritePool_sparseWriteEnd(ress->writeCtx);

    return outFileSize;
}
#endif

#ifdef ZSTD_LZ4COMPRESS

#if LZ4_VERSION_NUMBER <= 10600
#define LZ4F_blockLinked blockLinked
#define LZ4F_max64KB max64KB
#endif

static int FIO_LZ4_GetBlockSize_FromBlockId (int id) { return (1 << (8 + (2 * id))); }

static unsigned long long
FIO_compressLz4Frame(cRess_t* ress,
                     const char* srcFileName, U64 const srcFileSize,
                     int compressionLevel, int checksumFlag,
                     U64* readsize)
{
    const size_t blockSize = FIO_LZ4_GetBlockSize_FromBlockId(LZ4F_max64KB);
    unsigned long long inFileSize = 0, outFileSize = 0;

    LZ4F_preferences_t prefs;
    LZ4F_compressionContext_t ctx;

    IOJob_t* writeJob = AIO_WritePool_acquireJob(ress->writeCtx);

    LZ4F_errorCode_t const errorCode = LZ4F_createCompressionContext(&ctx, LZ4F_VERSION);
    if (LZ4F_isError(errorCode))
        EXM_THROW(31, "zstd: failed to create lz4 compression context");

    memset(&prefs, 0, sizeof(prefs));

    assert(blockSize <= ress->readCtx->base.jobBufferSize);

    /* autoflush off to mitigate a bug in lz4<=1.9.3 for compression level 12 */
    prefs.autoFlush = 0;
    prefs.compressionLevel = compressionLevel;
    prefs.frameInfo.blockMode = LZ4F_blockLinked;
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.contentChecksumFlag = (contentChecksum_t)checksumFlag;
#if LZ4_VERSION_NUMBER >= 10600
    prefs.frameInfo.contentSize = (srcFileSize==UTIL_FILESIZE_UNKNOWN) ? 0 : srcFileSize;
#endif
    assert(LZ4F_compressBound(blockSize, &prefs) <= writeJob->bufferSize);

    {
        size_t headerSize = LZ4F_compressBegin(ctx, writeJob->buffer, writeJob->bufferSize, &prefs);
        if (LZ4F_isError(headerSize))
            EXM_THROW(33, "File header generation failed : %s",
                            LZ4F_getErrorName(headerSize));
        writeJob->usedBufferSize = headerSize;
        AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
        outFileSize += headerSize;

        /* Read first block */
        inFileSize += AIO_ReadPool_fillBuffer(ress->readCtx, blockSize);

        /* Main Loop */
        while (ress->readCtx->srcBufferLoaded) {
            size_t inSize = MIN(blockSize, ress->readCtx->srcBufferLoaded);
            size_t const outSize = LZ4F_compressUpdate(ctx, writeJob->buffer, writeJob->bufferSize,
                                                       ress->readCtx->srcBuffer, inSize, NULL);
            if (LZ4F_isError(outSize))
                EXM_THROW(35, "zstd: %s: lz4 compression failed : %s",
                            srcFileName, LZ4F_getErrorName(outSize));
            outFileSize += outSize;
            if (srcFileSize == UTIL_FILESIZE_UNKNOWN) {
                DISPLAYUPDATE_PROGRESS("\rRead : %u MB ==> %.2f%%",
                                (unsigned)(inFileSize>>20),
                                (double)outFileSize/(double)inFileSize*100)
            } else {
                DISPLAYUPDATE_PROGRESS("\rRead : %u / %u MB ==> %.2f%%",
                                (unsigned)(inFileSize>>20), (unsigned)(srcFileSize>>20),
                                (double)outFileSize/(double)inFileSize*100);
            }

            /* Write Block */
            writeJob->usedBufferSize = outSize;
            AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);

            /* Read next block */
            AIO_ReadPool_consumeBytes(ress->readCtx, inSize);
            inFileSize += AIO_ReadPool_fillBuffer(ress->readCtx, blockSize);
        }

        /* End of Stream mark */
        headerSize = LZ4F_compressEnd(ctx, writeJob->buffer, writeJob->bufferSize, NULL);
        if (LZ4F_isError(headerSize))
            EXM_THROW(38, "zstd: %s: lz4 end of file generation failed : %s",
                        srcFileName, LZ4F_getErrorName(headerSize));

        writeJob->usedBufferSize = headerSize;
        AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
        outFileSize += headerSize;
    }

    *readsize = inFileSize;
    LZ4F_freeCompressionContext(ctx);
    AIO_WritePool_releaseIoJob(writeJob);
    AIO_WritePool_sparseWriteEnd(ress->writeCtx);

    return outFileSize;
}
#endif

static unsigned long long
FIO_compressZstdFrame(FIO_ctx_t* const fCtx,
                      FIO_prefs_t* const prefs,
                      const cRess_t* ressPtr,
                      const char* srcFileName, U64 fileSize,
                      int compressionLevel, U64* readsize)
{
    cRess_t const ress = *ressPtr;
    IOJob_t *writeJob = AIO_WritePool_acquireJob(ressPtr->writeCtx);

    U64 compressedfilesize = 0;
    ZSTD_EndDirective directive = ZSTD_e_continue;
    U64 pledgedSrcSize = ZSTD_CONTENTSIZE_UNKNOWN;

    /* stats */
    ZSTD_frameProgression previous_zfp_update = { 0, 0, 0, 0, 0, 0 };
    ZSTD_frameProgression previous_zfp_correction = { 0, 0, 0, 0, 0, 0 };
    typedef enum { noChange, slower, faster } speedChange_e;
    speedChange_e speedChange = noChange;
    unsigned flushWaiting = 0;
    unsigned inputPresented = 0;
    unsigned inputBlocked = 0;
    unsigned lastJobID = 0;
    UTIL_time_t lastAdaptTime = UTIL_getTime();
    U64 const adaptEveryMicro = REFRESH_RATE;

    UTIL_HumanReadableSize_t const file_hrs = UTIL_makeHumanReadableSize(fileSize);

    DISPLAYLEVEL(6, "compression using zstd format \n");

    /* init */
    if (fileSize != UTIL_FILESIZE_UNKNOWN) {
        pledgedSrcSize = fileSize;
        CHECK(ZSTD_CCtx_setPledgedSrcSize(ress.cctx, fileSize));
    } else if (prefs->streamSrcSize > 0) {
      /* unknown source size; use the declared stream size */
      pledgedSrcSize = prefs->streamSrcSize;
      CHECK( ZSTD_CCtx_setPledgedSrcSize(ress.cctx, prefs->streamSrcSize) );
    }

    {
        int windowLog;
        UTIL_HumanReadableSize_t windowSize;
        CHECK(ZSTD_CCtx_getParameter(ress.cctx, ZSTD_c_windowLog, &windowLog));
        if (windowLog == 0) {
            if (prefs->ldmFlag) {
                /* If long mode is set without a window size libzstd will set this size internally */
                windowLog = ZSTD_WINDOWLOG_LIMIT_DEFAULT;
            } else {
                const ZSTD_compressionParameters cParams = ZSTD_getCParams(compressionLevel, fileSize, 0);
                windowLog = (int)cParams.windowLog;
            }
        }
        windowSize = UTIL_makeHumanReadableSize(MAX(1ULL, MIN(1ULL << windowLog, pledgedSrcSize)));
        DISPLAYLEVEL(4, "Decompression will require %.*f%s of memory\n", windowSize.precision, windowSize.value, windowSize.suffix);
    }
    (void)srcFileName;

    /* Main compression loop */
    do {
        size_t stillToFlush;
        /* Fill input Buffer */
        size_t const inSize = AIO_ReadPool_fillBuffer(ress.readCtx, ZSTD_CStreamInSize());
        ZSTD_inBuffer inBuff = setInBuffer( ress.readCtx->srcBuffer, ress.readCtx->srcBufferLoaded, 0 );
        DISPLAYLEVEL(6, "fread %u bytes from source \n", (unsigned)inSize);
        *readsize += inSize;

        if ((ress.readCtx->srcBufferLoaded == 0) || (*readsize == fileSize))
            directive = ZSTD_e_end;

        stillToFlush = 1;
        while ((inBuff.pos != inBuff.size)   /* input buffer must be entirely ingested */
            || (directive == ZSTD_e_end && stillToFlush != 0) ) {

            size_t const oldIPos = inBuff.pos;
            ZSTD_outBuffer outBuff = setOutBuffer( writeJob->buffer, writeJob->bufferSize, 0 );
            size_t const toFlushNow = ZSTD_toFlushNow(ress.cctx);
            CHECK_V(stillToFlush, ZSTD_compressStream2(ress.cctx, &outBuff, &inBuff, directive));
            AIO_ReadPool_consumeBytes(ress.readCtx, inBuff.pos - oldIPos);

            /* count stats */
            inputPresented++;
            if (oldIPos == inBuff.pos) inputBlocked++;  /* input buffer is full and can't take any more : input speed is faster than consumption rate */
            if (!toFlushNow) flushWaiting = 1;

            /* Write compressed stream */
            DISPLAYLEVEL(6, "ZSTD_compress_generic(end:%u) => input pos(%u)<=(%u)size ; output generated %u bytes \n",
                         (unsigned)directive, (unsigned)inBuff.pos, (unsigned)inBuff.size, (unsigned)outBuff.pos);
            if (outBuff.pos) {
                writeJob->usedBufferSize = outBuff.pos;
                AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
                compressedfilesize += outBuff.pos;
            }

            /* adaptive mode : statistics measurement and speed correction */
            if (prefs->adaptiveMode && UTIL_clockSpanMicro(lastAdaptTime) > adaptEveryMicro) {
                ZSTD_frameProgression const zfp = ZSTD_getFrameProgression(ress.cctx);

                lastAdaptTime = UTIL_getTime();

                /* check output speed */
                if (zfp.currentJobID > 1) {  /* only possible if nbWorkers >= 1 */

                    unsigned long long newlyProduced = zfp.produced - previous_zfp_update.produced;
                    unsigned long long newlyFlushed = zfp.flushed - previous_zfp_update.flushed;
                    assert(zfp.produced >= previous_zfp_update.produced);
                    assert(prefs->nbWorkers >= 1);

                    /* test if compression is blocked
                        * either because output is slow and all buffers are full
                        * or because input is slow and no job can start while waiting for at least one buffer to be filled.
                        * note : exclude starting part, since currentJobID > 1 */
                    if ( (zfp.consumed == previous_zfp_update.consumed)   /* no data compressed : no data available, or no more buffer to compress to, OR compression is really slow (compression of a single block is slower than update rate)*/
                        && (zfp.nbActiveWorkers == 0)                       /* confirmed : no compression ongoing */
                        ) {
                        DISPLAYLEVEL(6, "all buffers full : compression stopped => slow down \n")
                        speedChange = slower;
                    }

                    previous_zfp_update = zfp;

                    if ( (newlyProduced > (newlyFlushed * 9 / 8))   /* compression produces more data than output can flush (though production can be spiky, due to work unit : (N==4)*block sizes) */
                        && (flushWaiting == 0)                        /* flush speed was never slowed by lack of production, so it's operating at max capacity */
                        ) {
                        DISPLAYLEVEL(6, "compression faster than flush (%llu > %llu), and flushed was never slowed down by lack of production => slow down \n", newlyProduced, newlyFlushed);
                        speedChange = slower;
                    }
                    flushWaiting = 0;
                }

                /* course correct only if there is at least one new job completed */
                if (zfp.currentJobID > lastJobID) {
                    DISPLAYLEVEL(6, "compression level adaptation check \n")

                    /* check input speed */
                    if (zfp.currentJobID > (unsigned)(prefs->nbWorkers+1)) {   /* warm up period, to fill all workers */
                        if (inputBlocked <= 0) {
                            DISPLAYLEVEL(6, "input is never blocked => input is slower than ingestion \n");
                            speedChange = slower;
                        } else if (speedChange == noChange) {
                            unsigned long long newlyIngested = zfp.ingested - previous_zfp_correction.ingested;
                            unsigned long long newlyConsumed = zfp.consumed - previous_zfp_correction.consumed;
                            unsigned long long newlyProduced = zfp.produced - previous_zfp_correction.produced;
                            unsigned long long newlyFlushed  = zfp.flushed  - previous_zfp_correction.flushed;
                            previous_zfp_correction = zfp;
                            assert(inputPresented > 0);
                            DISPLAYLEVEL(6, "input blocked %u/%u(%.2f) - ingested:%u vs %u:consumed - flushed:%u vs %u:produced \n",
                                            inputBlocked, inputPresented, (double)inputBlocked/inputPresented*100,
                                            (unsigned)newlyIngested, (unsigned)newlyConsumed,
                                            (unsigned)newlyFlushed, (unsigned)newlyProduced);
                            if ( (inputBlocked > inputPresented / 8)     /* input is waiting often, because input buffers is full : compression or output too slow */
                                && (newlyFlushed * 33 / 32 > newlyProduced)  /* flush everything that is produced */
                                && (newlyIngested * 33 / 32 > newlyConsumed) /* input speed as fast or faster than compression speed */
                            ) {
                                DISPLAYLEVEL(6, "recommend faster as in(%llu) >= (%llu)comp(%llu) <= out(%llu) \n",
                                                newlyIngested, newlyConsumed, newlyProduced, newlyFlushed);
                                speedChange = faster;
                            }
                        }
                        inputBlocked = 0;
                        inputPresented = 0;
                    }

                    if (speedChange == slower) {
                        DISPLAYLEVEL(6, "slower speed , higher compression \n")
                        compressionLevel ++;
                        if (compressionLevel > ZSTD_maxCLevel()) compressionLevel = ZSTD_maxCLevel();
                        if (compressionLevel > prefs->maxAdaptLevel) compressionLevel = prefs->maxAdaptLevel;
                        compressionLevel += (compressionLevel == 0);   /* skip 0 */
                        ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_compressionLevel, compressionLevel);
                    }
                    if (speedChange == faster) {
                        DISPLAYLEVEL(6, "faster speed , lighter compression \n")
                        compressionLevel --;
                        if (compressionLevel < prefs->minAdaptLevel) compressionLevel = prefs->minAdaptLevel;
                        compressionLevel -= (compressionLevel == 0);   /* skip 0 */
                        ZSTD_CCtx_setParameter(ress.cctx, ZSTD_c_compressionLevel, compressionLevel);
                    }
                    speedChange = noChange;

                    lastJobID = zfp.currentJobID;
                }  /* if (zfp.currentJobID > lastJobID) */
            } /* if (prefs->adaptiveMode && UTIL_clockSpanMicro(lastAdaptTime) > adaptEveryMicro) */

            /* display notification */
            if (SHOULD_DISPLAY_PROGRESS() && READY_FOR_UPDATE()) {
                ZSTD_frameProgression const zfp = ZSTD_getFrameProgression(ress.cctx);
                double const cShare = (double)zfp.produced / (double)(zfp.consumed + !zfp.consumed/*avoid div0*/) * 100;
                UTIL_HumanReadableSize_t const buffered_hrs = UTIL_makeHumanReadableSize(zfp.ingested - zfp.consumed);
                UTIL_HumanReadableSize_t const consumed_hrs = UTIL_makeHumanReadableSize(zfp.consumed);
                UTIL_HumanReadableSize_t const produced_hrs = UTIL_makeHumanReadableSize(zfp.produced);

                DELAY_NEXT_UPDATE();

                /* display progress notifications */
                DISPLAY_PROGRESS("\r%79s\r", "");    /* Clear out the current displayed line */
                if (g_display_prefs.displayLevel >= 3) {
                    /* Verbose progress update */
                    DISPLAY_PROGRESS(
                            "(L%i) Buffered:%5.*f%s - Consumed:%5.*f%s - Compressed:%5.*f%s => %.2f%% ",
                            compressionLevel,
                            buffered_hrs.precision, buffered_hrs.value, buffered_hrs.suffix,
                            consumed_hrs.precision, consumed_hrs.value, consumed_hrs.suffix,
                            produced_hrs.precision, produced_hrs.value, produced_hrs.suffix,
                            cShare );
                } else {
                    /* Require level 2 or forcibly displayed progress counter for summarized updates */
                    if (fCtx->nbFilesTotal > 1) {
                        size_t srcFileNameSize = strlen(srcFileName);
                        /* Ensure that the string we print is roughly the same size each time */
                        if (srcFileNameSize > 18) {
                            const char* truncatedSrcFileName = srcFileName + srcFileNameSize - 15;
                            DISPLAY_PROGRESS("Compress: %u/%u files. Current: ...%s ",
                                        fCtx->currFileIdx+1, fCtx->nbFilesTotal, truncatedSrcFileName);
                        } else {
                            DISPLAY_PROGRESS("Compress: %u/%u files. Current: %*s ",
                                        fCtx->currFileIdx+1, fCtx->nbFilesTotal, (int)(18-srcFileNameSize), srcFileName);
                        }
                    }
                    DISPLAY_PROGRESS("Read:%6.*f%4s ", consumed_hrs.precision, consumed_hrs.value, consumed_hrs.suffix);
                    if (fileSize != UTIL_FILESIZE_UNKNOWN)
                        DISPLAY_PROGRESS("/%6.*f%4s", file_hrs.precision, file_hrs.value, file_hrs.suffix);
                    DISPLAY_PROGRESS(" ==> %2.f%%", cShare);
                }
            }  /* if (SHOULD_DISPLAY_PROGRESS() && READY_FOR_UPDATE()) */
        }  /* while ((inBuff.pos != inBuff.size) */
    } while (directive != ZSTD_e_end);

    if (fileSize != UTIL_FILESIZE_UNKNOWN && *readsize != fileSize) {
        EXM_THROW(27, "Read error : Incomplete read : %llu / %llu B",
                (unsigned long long)*readsize, (unsigned long long)fileSize);
    }

    AIO_WritePool_releaseIoJob(writeJob);
    AIO_WritePool_sparseWriteEnd(ressPtr->writeCtx);

    return compressedfilesize;
}

/*! FIO_compressFilename_internal() :
 *  same as FIO_compressFilename_extRess(), with `ress.desFile` already opened.
 *  @return : 0 : compression completed correctly,
 *            1 : missing or pb opening srcFileName
 */
static int
FIO_compressFilename_internal(FIO_ctx_t* const fCtx,
                              FIO_prefs_t* const prefs,
                              cRess_t ress,
                              const char* dstFileName, const char* srcFileName,
                              int compressionLevel)
{
    UTIL_time_t const timeStart = UTIL_getTime();
    clock_t const cpuStart = clock();
    U64 readsize = 0;
    U64 compressedfilesize = 0;
    U64 const fileSize = UTIL_getFileSize(srcFileName);
    DISPLAYLEVEL(5, "%s: %llu bytes \n", srcFileName, (unsigned long long)fileSize);

    /* compression format selection */
    switch (prefs->compressionType) {
        default:
        case FIO_zstdCompression:
            compressedfilesize = FIO_compressZstdFrame(fCtx, prefs, &ress, srcFileName, fileSize, compressionLevel, &readsize);
            break;

        case FIO_gzipCompression:
#ifdef ZSTD_GZCOMPRESS
            compressedfilesize = FIO_compressGzFrame(&ress, srcFileName, fileSize, compressionLevel, &readsize);
#else
            (void)compressionLevel;
            EXM_THROW(20, "zstd: %s: file cannot be compressed as gzip (zstd compiled without ZSTD_GZCOMPRESS) -- ignored \n",
                            srcFileName);
#endif
            break;

        case FIO_xzCompression:
        case FIO_lzmaCompression:
#ifdef ZSTD_LZMACOMPRESS
            compressedfilesize = FIO_compressLzmaFrame(&ress, srcFileName, fileSize, compressionLevel, &readsize, prefs->compressionType==FIO_lzmaCompression);
#else
            (void)compressionLevel;
            EXM_THROW(20, "zstd: %s: file cannot be compressed as xz/lzma (zstd compiled without ZSTD_LZMACOMPRESS) -- ignored \n",
                            srcFileName);
#endif
            break;

        case FIO_lz4Compression:
#ifdef ZSTD_LZ4COMPRESS
            compressedfilesize = FIO_compressLz4Frame(&ress, srcFileName, fileSize, compressionLevel, prefs->checksumFlag, &readsize);
#else
            (void)compressionLevel;
            EXM_THROW(20, "zstd: %s: file cannot be compressed as lz4 (zstd compiled without ZSTD_LZ4COMPRESS) -- ignored \n",
                            srcFileName);
#endif
            break;
    }

    /* Status */
    fCtx->totalBytesInput += (size_t)readsize;
    fCtx->totalBytesOutput += (size_t)compressedfilesize;
    DISPLAY_PROGRESS("\r%79s\r", "");
    if (FIO_shouldDisplayFileSummary(fCtx)) {
        UTIL_HumanReadableSize_t hr_isize = UTIL_makeHumanReadableSize((U64) readsize);
        UTIL_HumanReadableSize_t hr_osize = UTIL_makeHumanReadableSize((U64) compressedfilesize);
        if (readsize == 0) {
            DISPLAY_SUMMARY("%-20s :  (%6.*f%s => %6.*f%s, %s) \n",
                srcFileName,
                hr_isize.precision, hr_isize.value, hr_isize.suffix,
                hr_osize.precision, hr_osize.value, hr_osize.suffix,
                dstFileName);
        } else {
            DISPLAY_SUMMARY("%-20s :%6.2f%%   (%6.*f%s => %6.*f%s, %s) \n",
                srcFileName,
                (double)compressedfilesize / (double)readsize * 100,
                hr_isize.precision, hr_isize.value, hr_isize.suffix,
                hr_osize.precision, hr_osize.value, hr_osize.suffix,
                dstFileName);
        }
    }

    /* Elapsed Time and CPU Load */
    {   clock_t const cpuEnd = clock();
        double const cpuLoad_s = (double)(cpuEnd - cpuStart) / CLOCKS_PER_SEC;
        U64 const timeLength_ns = UTIL_clockSpanNano(timeStart);
        double const timeLength_s = (double)timeLength_ns / 1000000000;
        double const cpuLoad_pct = (cpuLoad_s / timeLength_s) * 100;
        DISPLAYLEVEL(4, "%-20s : Completed in %.2f sec  (cpu load : %.0f%%)\n",
                        srcFileName, timeLength_s, cpuLoad_pct);
    }
    return 0;
}


/*! FIO_compressFilename_dstFile() :
 *  open dstFileName, or pass-through if ress.file != NULL,
 *  then start compression with FIO_compressFilename_internal().
 *  Manages source removal (--rm) and file permissions transfer.
 *  note : ress.srcFile must be != NULL,
 *  so reach this function through FIO_compressFilename_srcFile().
 *  @return : 0 : compression completed correctly,
 *            1 : pb
 */
static int FIO_compressFilename_dstFile(FIO_ctx_t* const fCtx,
                                        FIO_prefs_t* const prefs,
                                        cRess_t ress,
                                        const char* dstFileName,
                                        const char* srcFileName,
                                        const stat_t* srcFileStat,
                                        int compressionLevel)
{
    int closeDstFile = 0;
    int result;
    int transferStat = 0;
    FILE *dstFile;
    int dstFd = -1;

    assert(AIO_ReadPool_getFile(ress.readCtx) != NULL);
    if (AIO_WritePool_getFile(ress.writeCtx) == NULL) {
        int dstFileInitialPermissions = DEFAULT_FILE_PERMISSIONS;
        if ( strcmp (srcFileName, stdinmark)
          && strcmp (dstFileName, stdoutmark)
          && UTIL_isRegularFileStat(srcFileStat) ) {
            transferStat = 1;
            dstFileInitialPermissions = TEMPORARY_FILE_PERMISSIONS;
        }

        closeDstFile = 1;
        DISPLAYLEVEL(6, "FIO_compressFilename_dstFile: opening dst: %s \n", dstFileName);
        dstFile = FIO_openDstFile(fCtx, prefs, srcFileName, dstFileName, dstFileInitialPermissions);
        if (dstFile==NULL) return 1;  /* could not open dstFileName */
        dstFd = fileno(dstFile);
        AIO_WritePool_setFile(ress.writeCtx, dstFile);
        /* Must only be added after FIO_openDstFile() succeeds.
         * Otherwise we may delete the destination file if it already exists,
         * and the user presses Ctrl-C when asked if they wish to overwrite.
         */
        addHandler(dstFileName);
    }

    result = FIO_compressFilename_internal(fCtx, prefs, ress, dstFileName, srcFileName, compressionLevel);

    if (closeDstFile) {
        clearHandler();

        if (transferStat) {
            UTIL_setFDStat(dstFd, dstFileName, srcFileStat);
        }

        DISPLAYLEVEL(6, "FIO_compressFilename_dstFile: closing dst: %s \n", dstFileName);
        if (AIO_WritePool_closeFile(ress.writeCtx)) { /* error closing file */
            DISPLAYLEVEL(1, "zstd: %s: %s \n", dstFileName, strerror(errno));
            result=1;
        }

        if (transferStat) {
            UTIL_utime(dstFileName, srcFileStat);
        }

        if ( (result != 0)  /* operation failure */
          && strcmp(dstFileName, stdoutmark)  /* special case : don't remove() stdout */
          ) {
            FIO_removeFile(dstFileName); /* remove compression artefact; note don't do anything special if remove() fails */
        }
    }

    return result;
}

/* List used to compare file extensions (used with --exclude-compressed flag)
* Different from the suffixList and should only apply to ZSTD compress operationResult
*/
static const char *compressedFileExtensions[] = {
    ZSTD_EXTENSION,
    TZSTD_EXTENSION,
    GZ_EXTENSION,
    TGZ_EXTENSION,
    LZMA_EXTENSION,
    XZ_EXTENSION,
    TXZ_EXTENSION,
    LZ4_EXTENSION,
    TLZ4_EXTENSION,
    NULL
};

/*! FIO_compressFilename_srcFile() :
 *  @return : 0 : compression completed correctly,
 *            1 : missing or pb opening srcFileName
 */
static int
FIO_compressFilename_srcFile(FIO_ctx_t* const fCtx,
                             FIO_prefs_t* const prefs,
                             cRess_t ress,
                             const char* dstFileName,
                             const char* srcFileName,
                             int compressionLevel)
{
    int result;
    FILE* srcFile;
    stat_t srcFileStat;
    U64 fileSize = UTIL_FILESIZE_UNKNOWN;
    DISPLAYLEVEL(6, "FIO_compressFilename_srcFile: %s \n", srcFileName);

    if (strcmp(srcFileName, stdinmark)) {
        if (UTIL_stat(srcFileName, &srcFileStat)) {
            /* failure to stat at all is handled during opening */

            /* ensure src is not a directory */
            if (UTIL_isDirectoryStat(&srcFileStat)) {
                DISPLAYLEVEL(1, "zstd: %s is a directory -- ignored \n", srcFileName);
                return 1;
            }

            /* ensure src is not the same as dict (if present) */
            if (ress.dictFileName != NULL && UTIL_isSameFileStat(srcFileName, ress.dictFileName, &srcFileStat, &ress.dictFileStat)) {
                DISPLAYLEVEL(1, "zstd: cannot use %s as an input file and dictionary \n", srcFileName);
                return 1;
            }
        }
    }

    /* Check if "srcFile" is compressed. Only done if --exclude-compressed flag is used
    * YES => ZSTD will skip compression of the file and will return 0.
    * NO => ZSTD will resume with compress operation.
    */
    if (prefs->excludeCompressedFiles == 1 && UTIL_isCompressedFile(srcFileName, compressedFileExtensions)) {
        DISPLAYLEVEL(4, "File is already compressed : %s \n", srcFileName);
        return 0;
    }

    srcFile = FIO_openSrcFile(prefs, srcFileName, &srcFileStat);
    if (srcFile == NULL) return 1;   /* srcFile could not be opened */

    /* Don't use AsyncIO for small files */
    if (strcmp(srcFileName, stdinmark)) /* Stdin doesn't have stats */
        fileSize = UTIL_getFileSizeStat(&srcFileStat);
    if(fileSize != UTIL_FILESIZE_UNKNOWN && fileSize < ZSTD_BLOCKSIZE_MAX * 3) {
        AIO_ReadPool_setAsync(ress.readCtx, 0);
        AIO_WritePool_setAsync(ress.writeCtx, 0);
    } else {
        AIO_ReadPool_setAsync(ress.readCtx, 1);
        AIO_WritePool_setAsync(ress.writeCtx, 1);
    }

    AIO_ReadPool_setFile(ress.readCtx, srcFile);
    result = FIO_compressFilename_dstFile(
            fCtx, prefs, ress,
            dstFileName, srcFileName,
            &srcFileStat, compressionLevel);
    AIO_ReadPool_closeFile(ress.readCtx);

    if ( prefs->removeSrcFile  /* --rm */
      && result == 0           /* success */
      && strcmp(srcFileName, stdinmark)  /* exception : don't erase stdin */
      ) {
        /* We must clear the handler, since after this point calling it would
         * delete both the source and destination files.
         */
        clearHandler();
        if (FIO_removeFile(srcFileName))
            EXM_THROW(1, "zstd: %s: %s", srcFileName, strerror(errno));
    }
    return result;
}

static const char*
checked_index(const char* options[], size_t length, size_t index) {
    assert(index < length);
    /* Necessary to avoid warnings since -O3 will omit the above `assert` */
    (void) length;
    return options[index];
}

#define INDEX(options, index) checked_index((options), sizeof(options)  / sizeof(char*), (size_t)(index))

void FIO_displayCompressionParameters(const FIO_prefs_t* prefs)
{
    static const char* formatOptions[5] = {ZSTD_EXTENSION, GZ_EXTENSION, XZ_EXTENSION,
                                           LZMA_EXTENSION, LZ4_EXTENSION};
    static const char* sparseOptions[3] = {" --no-sparse", "", " --sparse"};
    static const char* checkSumOptions[3] = {" --no-check", "", " --check"};
    static const char* rowMatchFinderOptions[3] = {"", " --no-row-match-finder", " --row-match-finder"};
    static const char* compressLiteralsOptions[3] = {"", " --compress-literals", " --no-compress-literals"};

    assert(g_display_prefs.displayLevel >= 4);

    DISPLAY("--format=%s", formatOptions[prefs->compressionType]);
    DISPLAY("%s", INDEX(sparseOptions, prefs->sparseFileSupport));
    DISPLAY("%s", prefs->dictIDFlag ? "" : " --no-dictID");
    DISPLAY("%s", INDEX(checkSumOptions, prefs->checksumFlag));
    DISPLAY(" --block-size=%d", prefs->blockSize);
    if (prefs->adaptiveMode)
        DISPLAY(" --adapt=min=%d,max=%d", prefs->minAdaptLevel, prefs->maxAdaptLevel);
    DISPLAY("%s", INDEX(rowMatchFinderOptions, prefs->useRowMatchFinder));
    DISPLAY("%s", prefs->rsyncable ? " --rsyncable" : "");
    if (prefs->streamSrcSize)
        DISPLAY(" --stream-size=%u", (unsigned) prefs->streamSrcSize);
    if (prefs->srcSizeHint)
        DISPLAY(" --size-hint=%d", prefs->srcSizeHint);
    if (prefs->targetCBlockSize)
        DISPLAY(" --target-compressed-block-size=%u", (unsigned) prefs->targetCBlockSize);
    DISPLAY("%s", INDEX(compressLiteralsOptions, prefs->literalCompressionMode));
    DISPLAY(" --memory=%u", prefs->memLimit ? prefs->memLimit : 128 MB);
    DISPLAY(" --threads=%d", prefs->nbWorkers);
    DISPLAY("%s", prefs->excludeCompressedFiles ? " --exclude-compressed" : "");
    DISPLAY(" --%scontent-size", prefs->contentSize ? "" : "no-");
    DISPLAY("\n");
}

#undef INDEX

int FIO_compressFilename(FIO_ctx_t* const fCtx, FIO_prefs_t* const prefs, const char* dstFileName,
                         const char* srcFileName, const char* dictFileName,
                         int compressionLevel, ZSTD_compressionParameters comprParams)
{
    cRess_t ress = FIO_createCResources(prefs, dictFileName, UTIL_getFileSize(srcFileName), compressionLevel, comprParams);
    int const result = FIO_compressFilename_srcFile(fCtx, prefs, ress, dstFileName, srcFileName, compressionLevel);

#define DISPLAY_LEVEL_DEFAULT 2

    FIO_freeCResources(&ress);
    return result;
}

/* FIO_determineCompressedName() :
 * create a destination filename for compressed srcFileName.
 * @return a pointer to it.
 * This function never returns an error (it may abort() in case of pb)
 */
static const char*
FIO_determineCompressedName(const char* srcFileName, const char* outDirName, const char* suffix)
{
    static size_t dfnbCapacity = 0;
    static char* dstFileNameBuffer = NULL;   /* using static allocation : this function cannot be multi-threaded */
    char* outDirFilename = NULL;
    size_t sfnSize = strlen(srcFileName);
    size_t const srcSuffixLen = strlen(suffix);

    if(!strcmp(srcFileName, stdinmark)) {
        return stdoutmark;
    }

    if (outDirName) {
        outDirFilename = FIO_createFilename_fromOutDir(srcFileName, outDirName, srcSuffixLen);
        sfnSize = strlen(outDirFilename);
        assert(outDirFilename != NULL);
    }

    if (dfnbCapacity <= sfnSize+srcSuffixLen+1) {
        /* resize buffer for dstName */
        free(dstFileNameBuffer);
        dfnbCapacity = sfnSize + srcSuffixLen + 30;
        dstFileNameBuffer = (char*)malloc(dfnbCapacity);
        if (!dstFileNameBuffer) {
            EXM_THROW(30, "zstd: %s", strerror(errno));
        }
    }
    assert(dstFileNameBuffer != NULL);

    if (outDirFilename) {
        memcpy(dstFileNameBuffer, outDirFilename, sfnSize);
        free(outDirFilename);
    } else {
        memcpy(dstFileNameBuffer, srcFileName, sfnSize);
    }
    memcpy(dstFileNameBuffer+sfnSize, suffix, srcSuffixLen+1 /* Include terminating null */);
    return dstFileNameBuffer;
}

static unsigned long long FIO_getLargestFileSize(const char** inFileNames, unsigned nbFiles)
{
    size_t i;
    unsigned long long fileSize, maxFileSize = 0;
    for (i = 0; i < nbFiles; i++) {
        fileSize = UTIL_getFileSize(inFileNames[i]);
        maxFileSize = fileSize > maxFileSize ? fileSize : maxFileSize;
    }
    return maxFileSize;
}

/* FIO_compressMultipleFilenames() :
 * compress nbFiles files
 * into either one destination (outFileName),
 * or into one file each (outFileName == NULL, but suffix != NULL),
 * or into a destination folder (specified with -O)
 */
int FIO_compressMultipleFilenames(FIO_ctx_t* const fCtx,
                                  FIO_prefs_t* const prefs,
                                  const char** inFileNamesTable,
                                  const char* outMirroredRootDirName,
                                  const char* outDirName,
                                  const char* outFileName, const char* suffix,
                                  const char* dictFileName, int compressionLevel,
                                  ZSTD_compressionParameters comprParams)
{
    int status;
    int error = 0;
    cRess_t ress = FIO_createCResources(prefs, dictFileName,
        FIO_getLargestFileSize(inFileNamesTable, (unsigned)fCtx->nbFilesTotal),
        compressionLevel, comprParams);

    /* init */
    assert(outFileName != NULL || suffix != NULL);
    if (outFileName != NULL) {   /* output into a single destination (stdout typically) */
        FILE *dstFile;
        if (FIO_multiFilesConcatWarning(fCtx, prefs, outFileName, 1 /* displayLevelCutoff */)) {
            FIO_freeCResources(&ress);
            return 1;
        }
        dstFile = FIO_openDstFile(fCtx, prefs, NULL, outFileName, DEFAULT_FILE_PERMISSIONS);
        if (dstFile == NULL) {  /* could not open outFileName */
            error = 1;
        } else {
            AIO_WritePool_setFile(ress.writeCtx, dstFile);
            for (; fCtx->currFileIdx < fCtx->nbFilesTotal; ++fCtx->currFileIdx) {
                status = FIO_compressFilename_srcFile(fCtx, prefs, ress, outFileName, inFileNamesTable[fCtx->currFileIdx], compressionLevel);
                if (!status) fCtx->nbFilesProcessed++;
                error |= status;
            }
            if (AIO_WritePool_closeFile(ress.writeCtx))
                EXM_THROW(29, "Write error (%s) : cannot properly close %s",
                            strerror(errno), outFileName);
        }
    } else {
        if (outMirroredRootDirName)
            UTIL_mirrorSourceFilesDirectories(inFileNamesTable, (unsigned)fCtx->nbFilesTotal, outMirroredRootDirName);

        for (; fCtx->currFileIdx < fCtx->nbFilesTotal; ++fCtx->currFileIdx) {
            const char* const srcFileName = inFileNamesTable[fCtx->currFileIdx];
            const char* dstFileName = NULL;
            if (outMirroredRootDirName) {
                char* validMirroredDirName = UTIL_createMirroredDestDirName(srcFileName, outMirroredRootDirName);
                if (validMirroredDirName) {
                    dstFileName = FIO_determineCompressedName(srcFileName, validMirroredDirName, suffix);
                    free(validMirroredDirName);
                } else {
                    DISPLAYLEVEL(2, "zstd: --output-dir-mirror cannot compress '%s' into '%s' \n", srcFileName, outMirroredRootDirName);
                    error=1;
                    continue;
                }
            } else {
                dstFileName = FIO_determineCompressedName(srcFileName, outDirName, suffix);  /* cannot fail */
            }
            status = FIO_compressFilename_srcFile(fCtx, prefs, ress, dstFileName, srcFileName, compressionLevel);
            if (!status) fCtx->nbFilesProcessed++;
            error |= status;
        }

        if (outDirName)
            FIO_checkFilenameCollisions(inFileNamesTable , (unsigned)fCtx->nbFilesTotal);
    }

    if (FIO_shouldDisplayMultipleFileSummary(fCtx)) {
        UTIL_HumanReadableSize_t hr_isize = UTIL_makeHumanReadableSize((U64) fCtx->totalBytesInput);
        UTIL_HumanReadableSize_t hr_osize = UTIL_makeHumanReadableSize((U64) fCtx->totalBytesOutput);

        DISPLAY_PROGRESS("\r%79s\r", "");
        if (fCtx->totalBytesInput == 0) {
            DISPLAY_SUMMARY("%3d files compressed : (%6.*f%4s => %6.*f%4s)\n",
                            fCtx->nbFilesProcessed,
                            hr_isize.precision, hr_isize.value, hr_isize.suffix,
                            hr_osize.precision, hr_osize.value, hr_osize.suffix);
        } else {
            DISPLAY_SUMMARY("%3d files compressed : %.2f%% (%6.*f%4s => %6.*f%4s)\n",
                            fCtx->nbFilesProcessed,
                            (double)fCtx->totalBytesOutput/((double)fCtx->totalBytesInput)*100,
                            hr_isize.precision, hr_isize.value, hr_isize.suffix,
                            hr_osize.precision, hr_osize.value, hr_osize.suffix);
        }
    }

    FIO_freeCResources(&ress);
    return error;
}

#endif /* #ifndef ZSTD_NOCOMPRESS */



#ifndef ZSTD_NODECOMPRESS

/* **************************************************************************
 *  Decompression
 ***************************************************************************/
typedef struct {
    FIO_Dict_t dict;
    ZSTD_DStream* dctx;
    WritePoolCtx_t *writeCtx;
    ReadPoolCtx_t *readCtx;
} dRess_t;

static dRess_t FIO_createDResources(FIO_prefs_t* const prefs, const char* dictFileName)
{
    int useMMap = prefs->mmapDict == ZSTD_ps_enable;
    int forceNoUseMMap = prefs->mmapDict == ZSTD_ps_disable;
    stat_t statbuf;
    dRess_t ress;
    memset(&ress, 0, sizeof(ress));

    FIO_getDictFileStat(dictFileName, &statbuf);

    if (prefs->patchFromMode){
        U64 const dictSize = UTIL_getFileSizeStat(&statbuf);
        useMMap |= dictSize > prefs->memLimit;
        FIO_adjustMemLimitForPatchFromMode(prefs, dictSize, 0 /* just use the dict size */);
    }

    /* Allocation */
    ress.dctx = ZSTD_createDStream();
    if (ress.dctx==NULL)
        EXM_THROW(60, "Error: %s : can't create ZSTD_DStream", strerror(errno));
    CHECK( ZSTD_DCtx_setMaxWindowSize(ress.dctx, prefs->memLimit) );
    CHECK( ZSTD_DCtx_setParameter(ress.dctx, ZSTD_d_forceIgnoreChecksum, !prefs->checksumFlag));

    /* dictionary */
    {
        FIO_dictBufferType_t dictBufferType = (useMMap && !forceNoUseMMap) ? FIO_mmapDict : FIO_mallocDict;
        FIO_initDict(&ress.dict, dictFileName, prefs, &statbuf, dictBufferType);

        CHECK(ZSTD_DCtx_reset(ress.dctx, ZSTD_reset_session_only) );

        if (prefs->patchFromMode){
            CHECK(ZSTD_DCtx_refPrefix(ress.dctx, ress.dict.dictBuffer, ress.dict.dictBufferSize));
        } else {
            CHECK(ZSTD_DCtx_loadDictionary_byReference(ress.dctx, ress.dict.dictBuffer, ress.dict.dictBufferSize));
        }
    }

    ress.writeCtx = AIO_WritePool_create(prefs, ZSTD_DStreamOutSize());
    ress.readCtx = AIO_ReadPool_create(prefs, ZSTD_DStreamInSize());
    return ress;
}

static void FIO_freeDResources(dRess_t ress)
{
    FIO_freeDict(&(ress.dict));
    CHECK( ZSTD_freeDStream(ress.dctx) );
    AIO_WritePool_free(ress.writeCtx);
    AIO_ReadPool_free(ress.readCtx);
}

/* FIO_passThrough() : just copy input into output, for compatibility with gzip -df mode
 * @return : 0 (no error) */
static int FIO_passThrough(dRess_t *ress)
{
    size_t const blockSize = MIN(MIN(64 KB, ZSTD_DStreamInSize()), ZSTD_DStreamOutSize());
    IOJob_t *writeJob = AIO_WritePool_acquireJob(ress->writeCtx);
    AIO_ReadPool_fillBuffer(ress->readCtx, blockSize);

    while(ress->readCtx->srcBufferLoaded) {
        size_t writeSize;
        writeSize = MIN(blockSize, ress->readCtx->srcBufferLoaded);
        assert(writeSize <= writeJob->bufferSize);
        memcpy(writeJob->buffer, ress->readCtx->srcBuffer, writeSize);
        writeJob->usedBufferSize = writeSize;
        AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
        AIO_ReadPool_consumeBytes(ress->readCtx, writeSize);
        AIO_ReadPool_fillBuffer(ress->readCtx, blockSize);
    }
    assert(ress->readCtx->reachedEof);
    AIO_WritePool_releaseIoJob(writeJob);
    AIO_WritePool_sparseWriteEnd(ress->writeCtx);
    return 0;
}

/* FIO_zstdErrorHelp() :
 * detailed error message when requested window size is too large */
static void
FIO_zstdErrorHelp(const FIO_prefs_t* const prefs,
                  const dRess_t* ress,
                  size_t err,
                  const char* srcFileName)
{
    ZSTD_frameHeader header;

    /* Help message only for one specific error */
    if (ZSTD_getErrorCode(err) != ZSTD_error_frameParameter_windowTooLarge)
        return;

    /* Try to decode the frame header */
    err = ZSTD_getFrameHeader(&header, ress->readCtx->srcBuffer, ress->readCtx->srcBufferLoaded);
    if (err == 0) {
        unsigned long long const windowSize = header.windowSize;
        unsigned const windowLog = FIO_highbit64(windowSize) + ((windowSize & (windowSize - 1)) != 0);
        assert(prefs->memLimit > 0);
        DISPLAYLEVEL(1, "%s : Window size larger than maximum : %llu > %u \n",
                        srcFileName, windowSize, prefs->memLimit);
        if (windowLog <= ZSTD_WINDOWLOG_MAX) {
            unsigned const windowMB = (unsigned)((windowSize >> 20) + ((windowSize & ((1 MB) - 1)) != 0));
            assert(windowSize < (U64)(1ULL << 52));   /* ensure now overflow for windowMB */
            DISPLAYLEVEL(1, "%s : Use --long=%u or --memory=%uMB \n",
                            srcFileName, windowLog, windowMB);
            return;
    }   }
    DISPLAYLEVEL(1, "%s : Window log larger than ZSTD_WINDOWLOG_MAX=%u; not supported \n",
                    srcFileName, ZSTD_WINDOWLOG_MAX);
}

/** FIO_decompressFrame() :
 *  @return : size of decoded zstd frame, or an error code
 */
#define FIO_ERROR_FRAME_DECODING   ((unsigned long long)(-2))
static unsigned long long
FIO_decompressZstdFrame(FIO_ctx_t* const fCtx, dRess_t* ress,
                        const FIO_prefs_t* const prefs,
                        const char* srcFileName,
                        U64 alreadyDecoded)  /* for multi-frames streams */
{
    U64 frameSize = 0;
    IOJob_t *writeJob = AIO_WritePool_acquireJob(ress->writeCtx);

    /* display last 20 characters only */
    {   size_t const srcFileLength = strlen(srcFileName);
        if (srcFileLength>20) srcFileName += srcFileLength-20;
    }

    ZSTD_DCtx_reset(ress->dctx, ZSTD_reset_session_only);

    /* Header loading : ensures ZSTD_getFrameHeader() will succeed */
    AIO_ReadPool_fillBuffer(ress->readCtx, ZSTD_FRAMEHEADERSIZE_MAX);

    /* Main decompression Loop */
    while (1) {
        ZSTD_inBuffer  inBuff = setInBuffer( ress->readCtx->srcBuffer, ress->readCtx->srcBufferLoaded, 0 );
        ZSTD_outBuffer outBuff= setOutBuffer( writeJob->buffer, writeJob->bufferSize, 0 );
        size_t const readSizeHint = ZSTD_decompressStream(ress->dctx, &outBuff, &inBuff);
        UTIL_HumanReadableSize_t const hrs = UTIL_makeHumanReadableSize(alreadyDecoded+frameSize);
        if (ZSTD_isError(readSizeHint)) {
            DISPLAYLEVEL(1, "%s : Decoding error (36) : %s \n",
                            srcFileName, ZSTD_getErrorName(readSizeHint));
            FIO_zstdErrorHelp(prefs, ress, readSizeHint, srcFileName);
            AIO_WritePool_releaseIoJob(writeJob);
            return FIO_ERROR_FRAME_DECODING;
        }

        /* Write block */
        writeJob->usedBufferSize = outBuff.pos;
        AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
        frameSize += outBuff.pos;
        if (fCtx->nbFilesTotal > 1) {
            size_t srcFileNameSize = strlen(srcFileName);
            if (srcFileNameSize > 18) {
                const char* truncatedSrcFileName = srcFileName + srcFileNameSize - 15;
                DISPLAYUPDATE_PROGRESS(
                        "\rDecompress: %2u/%2u files. Current: ...%s : %.*f%s...    ",
                        fCtx->currFileIdx+1, fCtx->nbFilesTotal, truncatedSrcFileName, hrs.precision, hrs.value, hrs.suffix);
            } else {
                DISPLAYUPDATE_PROGRESS("\rDecompress: %2u/%2u files. Current: %s : %.*f%s...    ",
                            fCtx->currFileIdx+1, fCtx->nbFilesTotal, srcFileName, hrs.precision, hrs.value, hrs.suffix);
            }
        } else {
            DISPLAYUPDATE_PROGRESS("\r%-20.20s : %.*f%s...     ",
                            srcFileName, hrs.precision, hrs.value, hrs.suffix);
        }

        AIO_ReadPool_consumeBytes(ress->readCtx, inBuff.pos);

        if (readSizeHint == 0) break;   /* end of frame */

        /* Fill input buffer */
        {   size_t const toDecode = MIN(readSizeHint, ZSTD_DStreamInSize());  /* support large skippable frames */
            if (ress->readCtx->srcBufferLoaded < toDecode) {
                size_t const readSize = AIO_ReadPool_fillBuffer(ress->readCtx, toDecode);
                if (readSize==0) {
                    DISPLAYLEVEL(1, "%s : Read error (39) : premature end \n",
                                 srcFileName);
                    AIO_WritePool_releaseIoJob(writeJob);
                    return FIO_ERROR_FRAME_DECODING;
                }
            }   }   }

    AIO_WritePool_releaseIoJob(writeJob);
    AIO_WritePool_sparseWriteEnd(ress->writeCtx);

    return frameSize;
}


#ifdef ZSTD_GZDECOMPRESS
static unsigned long long
FIO_decompressGzFrame(dRess_t* ress, const char* srcFileName)
{
    unsigned long long outFileSize = 0;
    z_stream strm;
    int flush = Z_NO_FLUSH;
    int decodingError = 0;
    IOJob_t *writeJob = NULL;

    strm.zalloc = Z_NULL;
    strm.zfree = Z_NULL;
    strm.opaque = Z_NULL;
    strm.next_in = 0;
    strm.avail_in = 0;
    /* see https://www.zlib.net/manual.html */
    if (inflateInit2(&strm, 15 /* maxWindowLogSize */ + 16 /* gzip only */) != Z_OK)
        return FIO_ERROR_FRAME_DECODING;

    writeJob = AIO_WritePool_acquireJob(ress->writeCtx);
    strm.next_out = (Bytef*)writeJob->buffer;
    strm.avail_out = (uInt)writeJob->bufferSize;
    strm.avail_in = (uInt)ress->readCtx->srcBufferLoaded;
    strm.next_in = (z_const unsigned char*)ress->readCtx->srcBuffer;

    for ( ; ; ) {
        int ret;
        if (strm.avail_in == 0) {
            AIO_ReadPool_consumeAndRefill(ress->readCtx);
            if (ress->readCtx->srcBufferLoaded == 0) flush = Z_FINISH;
            strm.next_in = (z_const unsigned char*)ress->readCtx->srcBuffer;
            strm.avail_in = (uInt)ress->readCtx->srcBufferLoaded;
        }
        ret = inflate(&strm, flush);
        if (ret == Z_BUF_ERROR) {
            DISPLAYLEVEL(1, "zstd: %s: premature gz end \n", srcFileName);
            decodingError = 1; break;
        }
        if (ret != Z_OK && ret != Z_STREAM_END) {
            DISPLAYLEVEL(1, "zstd: %s: inflate error %d \n", srcFileName, ret);
            decodingError = 1; break;
        }
        {   size_t const decompBytes = writeJob->bufferSize - strm.avail_out;
            if (decompBytes) {
                writeJob->usedBufferSize = decompBytes;
                AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
                outFileSize += decompBytes;
                strm.next_out = (Bytef*)writeJob->buffer;
                strm.avail_out = (uInt)writeJob->bufferSize;
            }
        }
        if (ret == Z_STREAM_END) break;
    }

    AIO_ReadPool_consumeBytes(ress->readCtx, ress->readCtx->srcBufferLoaded - strm.avail_in);

    if ( (inflateEnd(&strm) != Z_OK)  /* release resources ; error detected */
      && (decodingError==0) ) {
        DISPLAYLEVEL(1, "zstd: %s: inflateEnd error \n", srcFileName);
        decodingError = 1;
    }
    AIO_WritePool_releaseIoJob(writeJob);
    AIO_WritePool_sparseWriteEnd(ress->writeCtx);
    return decodingError ? FIO_ERROR_FRAME_DECODING : outFileSize;
}
#endif

#ifdef ZSTD_LZMADECOMPRESS
static unsigned long long
FIO_decompressLzmaFrame(dRess_t* ress,
                        const char* srcFileName, int plain_lzma)
{
    unsigned long long outFileSize = 0;
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_action action = LZMA_RUN;
    lzma_ret initRet;
    int decodingError = 0;
    IOJob_t *writeJob = NULL;

    strm.next_in = 0;
    strm.avail_in = 0;
    if (plain_lzma) {
        initRet = lzma_alone_decoder(&strm, UINT64_MAX); /* LZMA */
    } else {
        initRet = lzma_stream_decoder(&strm, UINT64_MAX, 0); /* XZ */
    }

    if (initRet != LZMA_OK) {
        DISPLAYLEVEL(1, "zstd: %s: %s error %d \n",
                        plain_lzma ? "lzma_alone_decoder" : "lzma_stream_decoder",
                        srcFileName, initRet);
        return FIO_ERROR_FRAME_DECODING;
    }

    writeJob = AIO_WritePool_acquireJob(ress->writeCtx);
    strm.next_out = (BYTE*)writeJob->buffer;
    strm.avail_out = writeJob->bufferSize;
    strm.next_in = (BYTE const*)ress->readCtx->srcBuffer;
    strm.avail_in = ress->readCtx->srcBufferLoaded;

    for ( ; ; ) {
        lzma_ret ret;
        if (strm.avail_in == 0) {
            AIO_ReadPool_consumeAndRefill(ress->readCtx);
            if (ress->readCtx->srcBufferLoaded == 0) action = LZMA_FINISH;
            strm.next_in = (BYTE const*)ress->readCtx->srcBuffer;
            strm.avail_in = ress->readCtx->srcBufferLoaded;
        }
        ret = lzma_code(&strm, action);

        if (ret == LZMA_BUF_ERROR) {
            DISPLAYLEVEL(1, "zstd: %s: premature lzma end \n", srcFileName);
            decodingError = 1; break;
        }
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
            DISPLAYLEVEL(1, "zstd: %s: lzma_code decoding error %d \n",
                            srcFileName, ret);
            decodingError = 1; break;
        }
        {   size_t const decompBytes = writeJob->bufferSize - strm.avail_out;
            if (decompBytes) {
                writeJob->usedBufferSize = decompBytes;
                AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
                outFileSize += decompBytes;
                strm.next_out = (BYTE*)writeJob->buffer;
                strm.avail_out = writeJob->bufferSize;
        }   }
        if (ret == LZMA_STREAM_END) break;
    }

    AIO_ReadPool_consumeBytes(ress->readCtx, ress->readCtx->srcBufferLoaded - strm.avail_in);
    lzma_end(&strm);
    AIO_WritePool_releaseIoJob(writeJob);
    AIO_WritePool_sparseWriteEnd(ress->writeCtx);
    return decodingError ? FIO_ERROR_FRAME_DECODING : outFileSize;
}
#endif

#ifdef ZSTD_LZ4DECOMPRESS
static unsigned long long
FIO_decompressLz4Frame(dRess_t* ress, const char* srcFileName)
{
    unsigned long long filesize = 0;
    LZ4F_errorCode_t nextToLoad = 4;
    LZ4F_decompressionContext_t dCtx;
    LZ4F_errorCode_t const errorCode = LZ4F_createDecompressionContext(&dCtx, LZ4F_VERSION);
    int decodingError = 0;
    IOJob_t *writeJob = NULL;

    if (LZ4F_isError(errorCode)) {
        DISPLAYLEVEL(1, "zstd: failed to create lz4 decompression context \n");
        return FIO_ERROR_FRAME_DECODING;
    }

    writeJob = AIO_WritePool_acquireJob(ress->writeCtx);

    /* Main Loop */
    for (;nextToLoad;) {
        size_t pos = 0;
        size_t decodedBytes = writeJob->bufferSize;
        int fullBufferDecoded = 0;

        /* Read input */
        AIO_ReadPool_fillBuffer(ress->readCtx, nextToLoad);
        if(!ress->readCtx->srcBufferLoaded) break; /* reached end of file */

        while ((pos < ress->readCtx->srcBufferLoaded) || fullBufferDecoded) {  /* still to read, or still to flush */
            /* Decode Input (at least partially) */
            size_t remaining = ress->readCtx->srcBufferLoaded - pos;
            decodedBytes = writeJob->bufferSize;
            nextToLoad = LZ4F_decompress(dCtx, writeJob->buffer, &decodedBytes, (char*)(ress->readCtx->srcBuffer)+pos,
                                         &remaining, NULL);
            if (LZ4F_isError(nextToLoad)) {
                DISPLAYLEVEL(1, "zstd: %s: lz4 decompression error : %s \n",
                                srcFileName, LZ4F_getErrorName(nextToLoad));
                decodingError = 1; nextToLoad = 0; break;
            }
            pos += remaining;
            assert(pos <= ress->readCtx->srcBufferLoaded);
            fullBufferDecoded = decodedBytes == writeJob->bufferSize;

            /* Write Block */
            if (decodedBytes) {
                UTIL_HumanReadableSize_t hrs;
                writeJob->usedBufferSize = decodedBytes;
                AIO_WritePool_enqueueAndReacquireWriteJob(&writeJob);
                filesize += decodedBytes;
                hrs = UTIL_makeHumanReadableSize(filesize);
                DISPLAYUPDATE_PROGRESS("\rDecompressed : %.*f%s  ", hrs.precision, hrs.value, hrs.suffix);
            }

            if (!nextToLoad) break;
        }
        AIO_ReadPool_consumeBytes(ress->readCtx, pos);
    }
    if (nextToLoad!=0) {
        DISPLAYLEVEL(1, "zstd: %s: unfinished lz4 stream \n", srcFileName);
        decodingError=1;
    }

    LZ4F_freeDecompressionContext(dCtx);
    AIO_WritePool_releaseIoJob(writeJob);
    AIO_WritePool_sparseWriteEnd(ress->writeCtx);

    return decodingError ? FIO_ERROR_FRAME_DECODING : filesize;
}
#endif



/** FIO_decompressFrames() :
 *  Find and decode frames inside srcFile
 *  srcFile presumed opened and valid
 * @return : 0 : OK
 *           1 : error
 */
static int FIO_decompressFrames(FIO_ctx_t* const fCtx,
                                dRess_t ress, const FIO_prefs_t* const prefs,
                                const char* dstFileName, const char* srcFileName)
{
    unsigned readSomething = 0;
    unsigned long long filesize = 0;
    int passThrough = prefs->passThrough;

    if (passThrough == -1) {
        /* If pass-through mode is not explicitly enabled or disabled,
         * default to the legacy behavior of enabling it if we are writing
         * to stdout with the overwrite flag enabled.
         */
        passThrough = prefs->overwrite && !strcmp(dstFileName, stdoutmark);
    }
    assert(passThrough == 0 || passThrough == 1);

    /* for each frame */
    for ( ; ; ) {
        /* check magic number -> version */
        size_t const toRead = 4;
        const BYTE* buf;
        AIO_ReadPool_fillBuffer(ress.readCtx, toRead);
        buf = (const BYTE*)ress.readCtx->srcBuffer;
        if (ress.readCtx->srcBufferLoaded==0) {
            if (readSomething==0) {  /* srcFile is empty (which is invalid) */
                DISPLAYLEVEL(1, "zstd: %s: unexpected end of file \n", srcFileName);
                return 1;
            }  /* else, just reached frame boundary */
            break;   /* no more input */
        }
        readSomething = 1;   /* there is at least 1 byte in srcFile */
        if (ress.readCtx->srcBufferLoaded < toRead) { /* not enough input to check magic number */
            if (passThrough) {
                return FIO_passThrough(&ress);
            }
            DISPLAYLEVEL(1, "zstd: %s: unknown header \n", srcFileName);
            return 1;
        }
        if (ZSTD_isFrame(buf, ress.readCtx->srcBufferLoaded)) {
            unsigned long long const frameSize = FIO_decompressZstdFrame(fCtx, &ress, prefs, srcFileName, filesize);
            if (frameSize == FIO_ERROR_FRAME_DECODING) return 1;
            filesize += frameSize;
        } else if (buf[0] == 31 && buf[1] == 139) { /* gz magic number */
#ifdef ZSTD_GZDECOMPRESS
            unsigned long long const frameSize = FIO_decompressGzFrame(&ress, srcFileName);
            if (frameSize == FIO_ERROR_FRAME_DECODING) return 1;
            filesize += frameSize;
#else
            DISPLAYLEVEL(1, "zstd: %s: gzip file cannot be uncompressed (zstd compiled without HAVE_ZLIB) -- ignored \n", srcFileName);
            return 1;
#endif
        } else if ((buf[0] == 0xFD && buf[1] == 0x37)  /* xz magic number */
                || (buf[0] == 0x5D && buf[1] == 0x00)) { /* lzma header (no magic number) */
#ifdef ZSTD_LZMADECOMPRESS
            unsigned long long const frameSize = FIO_decompressLzmaFrame(&ress, srcFileName, buf[0] != 0xFD);
            if (frameSize == FIO_ERROR_FRAME_DECODING) return 1;
            filesize += frameSize;
#else
            DISPLAYLEVEL(1, "zstd: %s: xz/lzma file cannot be uncompressed (zstd compiled without HAVE_LZMA) -- ignored \n", srcFileName);
            return 1;
#endif
        } else if (MEM_readLE32(buf) == LZ4_MAGICNUMBER) {
#ifdef ZSTD_LZ4DECOMPRESS
            unsigned long long const frameSize = FIO_decompressLz4Frame(&ress, srcFileName);
            if (frameSize == FIO_ERROR_FRAME_DECODING) return 1;
            filesize += frameSize;
#else
            DISPLAYLEVEL(1, "zstd: %s: lz4 file cannot be uncompressed (zstd compiled without HAVE_LZ4) -- ignored \n", srcFileName);
            return 1;
#endif
        } else if (passThrough) {
            return FIO_passThrough(&ress);
        } else {
            DISPLAYLEVEL(1, "zstd: %s: unsupported format \n", srcFileName);
            return 1;
    }   }  /* for each frame */

    /* Final Status */
    fCtx->totalBytesOutput += (size_t)filesize;
    DISPLAY_PROGRESS("\r%79s\r", "");
    if (FIO_shouldDisplayFileSummary(fCtx))
        DISPLAY_SUMMARY("%-20s: %llu bytes \n", srcFileName, filesize);

    return 0;
}

/** FIO_decompressDstFile() :
    open `dstFileName`, or pass-through if writeCtx's file is already != 0,
    then start decompression process (FIO_decompressFrames()).
    @return : 0 : OK
              1 : operation aborted
*/
static int FIO_decompressDstFile(FIO_ctx_t* const fCtx,
                                 FIO_prefs_t* const prefs,
                                 dRess_t ress,
                                 const char* dstFileName,
                                 const char* srcFileName,
                                 const stat_t* srcFileStat)
{
    int result;
    int releaseDstFile = 0;
    int transferStat = 0;
    int dstFd = 0;

    if ((AIO_WritePool_getFile(ress.writeCtx) == NULL) && (prefs->testMode == 0)) {
        FILE *dstFile;
        int dstFilePermissions = DEFAULT_FILE_PERMISSIONS;
        if ( strcmp(srcFileName, stdinmark)   /* special case : don't transfer permissions from stdin */
          && strcmp(dstFileName, stdoutmark)
          && UTIL_isRegularFileStat(srcFileStat) ) {
            transferStat = 1;
            dstFilePermissions = TEMPORARY_FILE_PERMISSIONS;
        }

        releaseDstFile = 1;

        dstFile = FIO_openDstFile(fCtx, prefs, srcFileName, dstFileName, dstFilePermissions);
        if (dstFile==NULL) return 1;
        dstFd = fileno(dstFile);
        AIO_WritePool_setFile(ress.writeCtx, dstFile);

        /* Must only be added after FIO_openDstFile() succeeds.
         * Otherwise we may delete the destination file if it already exists,
         * and the user presses Ctrl-C when asked if they wish to overwrite.
         */
        addHandler(dstFileName);
    }

    result = FIO_decompressFrames(fCtx, ress, prefs, dstFileName, srcFileName);

    if (releaseDstFile) {
        clearHandler();

        if (transferStat) {
            UTIL_setFDStat(dstFd, dstFileName, srcFileStat);
        }

        if (AIO_WritePool_closeFile(ress.writeCtx)) {
            DISPLAYLEVEL(1, "zstd: %s: %s \n", dstFileName, strerror(errno));
            result = 1;
        }

        if (transferStat) {
            UTIL_utime(dstFileName, srcFileStat);
        }

        if ( (result != 0)  /* operation failure */
          && strcmp(dstFileName, stdoutmark)  /* special case : don't remove() stdout */
          ) {
            FIO_removeFile(dstFileName);  /* remove decompression artefact; note: don't do anything special if remove() fails */
        }
    }

    return result;
}


/** FIO_decompressSrcFile() :
    Open `srcFileName`, transfer control to decompressDstFile()
    @return : 0 : OK
              1 : error
*/
static int FIO_decompressSrcFile(FIO_ctx_t* const fCtx, FIO_prefs_t* const prefs, dRess_t ress, const char* dstFileName, const char* srcFileName)
{
    FILE* srcFile;
    stat_t srcFileStat;
    int result;
    U64 fileSize = UTIL_FILESIZE_UNKNOWN;

    if (UTIL_isDirectory(srcFileName)) {
        DISPLAYLEVEL(1, "zstd: %s is a directory -- ignored \n", srcFileName);
        return 1;
    }

    srcFile = FIO_openSrcFile(prefs, srcFileName, &srcFileStat);
    if (srcFile==NULL) return 1;

    /* Don't use AsyncIO for small files */
    if (strcmp(srcFileName, stdinmark)) /* Stdin doesn't have stats */
        fileSize = UTIL_getFileSizeStat(&srcFileStat);
    if(fileSize != UTIL_FILESIZE_UNKNOWN && fileSize < ZSTD_BLOCKSIZE_MAX * 3) {
        AIO_ReadPool_setAsync(ress.readCtx, 0);
        AIO_WritePool_setAsync(ress.writeCtx, 0);
    } else {
        AIO_ReadPool_setAsync(ress.readCtx, 1);
        AIO_WritePool_setAsync(ress.writeCtx, 1);
    }

    AIO_ReadPool_setFile(ress.readCtx, srcFile);

    result = FIO_decompressDstFile(fCtx, prefs, ress, dstFileName, srcFileName, &srcFileStat);

    AIO_ReadPool_setFile(ress.readCtx, NULL);

    /* Close file */
    if (fclose(srcFile)) {
        DISPLAYLEVEL(1, "zstd: %s: %s \n", srcFileName, strerror(errno));  /* error should not happen */
        return 1;
    }
    if ( prefs->removeSrcFile  /* --rm */
      && (result==0)      /* decompression successful */
      && strcmp(srcFileName, stdinmark) ) /* not stdin */ {
        /* We must clear the handler, since after this point calling it would
         * delete both the source and destination files.
         */
        clearHandler();
        if (FIO_removeFile(srcFileName)) {
            /* failed to remove src file */
            DISPLAYLEVEL(1, "zstd: %s: %s \n", srcFileName, strerror(errno));
            return 1;
    }   }
    return result;
}



int FIO_decompressFilename(FIO_ctx_t* const fCtx, FIO_prefs_t* const prefs,
                           const char* dstFileName, const char* srcFileName,
                           const char* dictFileName)
{
    dRess_t const ress = FIO_createDResources(prefs, dictFileName);

    int const decodingError = FIO_decompressSrcFile(fCtx, prefs, ress, dstFileName, srcFileName);



    FIO_freeDResources(ress);
    return decodingError;
}

static const char *suffixList[] = {
    ZSTD_EXTENSION,
    TZSTD_EXTENSION,
#ifndef ZSTD_NODECOMPRESS
    ZSTD_ALT_EXTENSION,
#endif
#ifdef ZSTD_GZDECOMPRESS
    GZ_EXTENSION,
    TGZ_EXTENSION,
#endif
#ifdef ZSTD_LZMADECOMPRESS
    LZMA_EXTENSION,
    XZ_EXTENSION,
    TXZ_EXTENSION,
#endif
#ifdef ZSTD_LZ4DECOMPRESS
    LZ4_EXTENSION,
    TLZ4_EXTENSION,
#endif
    NULL
};

static const char *suffixListStr =
    ZSTD_EXTENSION "/" TZSTD_EXTENSION
#ifdef ZSTD_GZDECOMPRESS
    "/" GZ_EXTENSION "/" TGZ_EXTENSION
#endif
#ifdef ZSTD_LZMADECOMPRESS
    "/" LZMA_EXTENSION "/" XZ_EXTENSION "/" TXZ_EXTENSION
#endif
#ifdef ZSTD_LZ4DECOMPRESS
    "/" LZ4_EXTENSION "/" TLZ4_EXTENSION
#endif
;

/* FIO_determineDstName() :
 * create a destination filename from a srcFileName.
 * @return a pointer to it.
 * @return == NULL if there is an error */
static const char*
FIO_determineDstName(const char* srcFileName, const char* outDirName)
{
    static size_t dfnbCapacity = 0;
    static char* dstFileNameBuffer = NULL;   /* using static allocation : this function cannot be multi-threaded */
    size_t dstFileNameEndPos;
    char* outDirFilename = NULL;
    const char* dstSuffix = "";
    size_t dstSuffixLen = 0;

    size_t sfnSize = strlen(srcFileName);

    size_t srcSuffixLen;
    const char* const srcSuffix = strrchr(srcFileName, '.');

    if(!strcmp(srcFileName, stdinmark)) {
        return stdoutmark;
    }

    if (srcSuffix == NULL) {
        DISPLAYLEVEL(1,
            "zstd: %s: unknown suffix (%s expected). "
            "Can't derive the output file name. "
            "Specify it with -o dstFileName. Ignoring.\n",
            srcFileName, suffixListStr);
        return NULL;
    }
    srcSuffixLen = strlen(srcSuffix);

    {
        const char** matchedSuffixPtr;
        for (matchedSuffixPtr = suffixList; *matchedSuffixPtr != NULL; matchedSuffixPtr++) {
            if (!strcmp(*matchedSuffixPtr, srcSuffix)) {
                break;
            }
        }

        /* check suffix is authorized */
        if (sfnSize <= srcSuffixLen || *matchedSuffixPtr == NULL) {
            DISPLAYLEVEL(1,
                "zstd: %s: unknown suffix (%s expected). "
                "Can't derive the output file name. "
                "Specify it with -o dstFileName. Ignoring.\n",
                srcFileName, suffixListStr);
            return NULL;
        }

        if ((*matchedSuffixPtr)[1] == 't') {
            dstSuffix = ".tar";
            dstSuffixLen = strlen(dstSuffix);
        }
    }

    if (outDirName) {
        outDirFilename = FIO_createFilename_fromOutDir(srcFileName, outDirName, 0);
        sfnSize = strlen(outDirFilename);
        assert(outDirFilename != NULL);
    }

    if (dfnbCapacity+srcSuffixLen <= sfnSize+1+dstSuffixLen) {
        /* allocate enough space to write dstFilename into it */
        free(dstFileNameBuffer);
        dfnbCapacity = sfnSize + 20;
        dstFileNameBuffer = (char*)malloc(dfnbCapacity);
        if (dstFileNameBuffer==NULL)
            EXM_THROW(74, "%s : not enough memory for dstFileName",
                      strerror(errno));
    }

    /* return dst name == src name truncated from suffix */
    assert(dstFileNameBuffer != NULL);
    dstFileNameEndPos = sfnSize - srcSuffixLen;
    if (outDirFilename) {
        memcpy(dstFileNameBuffer, outDirFilename, dstFileNameEndPos);
        free(outDirFilename);
    } else {
        memcpy(dstFileNameBuffer, srcFileName, dstFileNameEndPos);
    }

    /* The short tar extensions tzst, tgz, txz and tlz4 files should have "tar"
     * extension on decompression. Also writes terminating null. */
    strcpy(dstFileNameBuffer + dstFileNameEndPos, dstSuffix);
    return dstFileNameBuffer;

    /* note : dstFileNameBuffer memory is not going to be free */
}

int
FIO_decompressMultipleFilenames(FIO_ctx_t* const fCtx,
                                FIO_prefs_t* const prefs,
                                const char** srcNamesTable,
                                const char* outMirroredRootDirName,
                                const char* outDirName, const char* outFileName,
                                const char* dictFileName)
{
    int status;
    int error = 0;
    dRess_t ress = FIO_createDResources(prefs, dictFileName);

    if (outFileName) {
        if (FIO_multiFilesConcatWarning(fCtx, prefs, outFileName, 1 /* displayLevelCutoff */)) {
            FIO_freeDResources(ress);
            return 1;
        }
        if (!prefs->testMode) {
            FILE* dstFile = FIO_openDstFile(fCtx, prefs, NULL, outFileName, DEFAULT_FILE_PERMISSIONS);
            if (dstFile == 0) EXM_THROW(19, "cannot open %s", outFileName);
            AIO_WritePool_setFile(ress.writeCtx, dstFile);
        }
        for (; fCtx->currFileIdx < fCtx->nbFilesTotal; fCtx->currFileIdx++) {
            status = FIO_decompressSrcFile(fCtx, prefs, ress, outFileName, srcNamesTable[fCtx->currFileIdx]);
            if (!status) fCtx->nbFilesProcessed++;
            error |= status;
        }
        if ((!prefs->testMode) && (AIO_WritePool_closeFile(ress.writeCtx)))
            EXM_THROW(72, "Write error : %s : cannot properly close output file",
                        strerror(errno));
    } else {
        if (outMirroredRootDirName)
            UTIL_mirrorSourceFilesDirectories(srcNamesTable, (unsigned)fCtx->nbFilesTotal, outMirroredRootDirName);

        for (; fCtx->currFileIdx < fCtx->nbFilesTotal; fCtx->currFileIdx++) {   /* create dstFileName */
            const char* const srcFileName = srcNamesTable[fCtx->currFileIdx];
            const char* dstFileName = NULL;
            if (outMirroredRootDirName) {
                char* validMirroredDirName = UTIL_createMirroredDestDirName(srcFileName, outMirroredRootDirName);
                if (validMirroredDirName) {
                    dstFileName = FIO_determineDstName(srcFileName, validMirroredDirName);
                    free(validMirroredDirName);
                } else {
                    DISPLAYLEVEL(2, "zstd: --output-dir-mirror cannot decompress '%s' into '%s'\n", srcFileName, outMirroredRootDirName);
                }
            } else {
                dstFileName = FIO_determineDstName(srcFileName, outDirName);
            }
            if (dstFileName == NULL) { error=1; continue; }
            status = FIO_decompressSrcFile(fCtx, prefs, ress, dstFileName, srcFileName);
            if (!status) fCtx->nbFilesProcessed++;
            error |= status;
        }
        if (outDirName)
            FIO_checkFilenameCollisions(srcNamesTable , (unsigned)fCtx->nbFilesTotal);
    }

    if (FIO_shouldDisplayMultipleFileSummary(fCtx)) {
        DISPLAY_PROGRESS("\r%79s\r", "");
        DISPLAY_SUMMARY("%d files decompressed : %6llu bytes total \n",
            fCtx->nbFilesProcessed, (unsigned long long)fCtx->totalBytesOutput);
    }

    FIO_freeDResources(ress);
    return error;
}

/* **************************************************************************
 *  .zst file info (--list command)
 ***************************************************************************/

typedef struct {
    U64 decompressedSize;
    U64 compressedSize;
    U64 windowSize;
    int numActualFrames;
    int numSkippableFrames;
    int decompUnavailable;
    int usesCheck;
    BYTE checksum[4];
    U32 nbFiles;
    unsigned dictID;
} fileInfo_t;

typedef enum {
  info_success=0,
  info_frame_error=1,
  info_not_zstd=2,
  info_file_error=3,
  info_truncated_input=4
} InfoError;

#define ERROR_IF(c,n,...) {             \
    if (c) {                           \
        DISPLAYLEVEL(1, __VA_ARGS__);  \
        DISPLAYLEVEL(1, " \n");        \
        return n;                      \
    }                                  \
}

static InfoError
FIO_analyzeFrames(fileInfo_t* info, FILE* const srcFile)
{
    /* begin analyzing frame */
    for ( ; ; ) {
        BYTE headerBuffer[ZSTD_FRAMEHEADERSIZE_MAX];
        size_t const numBytesRead = fread(headerBuffer, 1, sizeof(headerBuffer), srcFile);
        if (numBytesRead < ZSTD_FRAMEHEADERSIZE_MIN(ZSTD_f_zstd1)) {
            if ( feof(srcFile)
              && (numBytesRead == 0)
              && (info->compressedSize > 0)
              && (info->compressedSize != UTIL_FILESIZE_UNKNOWN) ) {
                unsigned long long file_position = (unsigned long long) LONG_TELL(srcFile);
                unsigned long long file_size = (unsigned long long) info->compressedSize;
                ERROR_IF(file_position != file_size, info_truncated_input,
                  "Error: seeked to position %llu, which is beyond file size of %llu\n",
                  file_position,
                  file_size);
                break;  /* correct end of file => success */
            }
            ERROR_IF(feof(srcFile), info_not_zstd, "Error: reached end of file with incomplete frame");
            ERROR_IF(1, info_frame_error, "Error: did not reach end of file but ran out of frames");
        }
        {   U32 const magicNumber = MEM_readLE32(headerBuffer);
            /* Zstandard frame */
            if (magicNumber == ZSTD_MAGICNUMBER) {
                ZSTD_frameHeader header;
                U64 const frameContentSize = ZSTD_getFrameContentSize(headerBuffer, numBytesRead);
                if ( frameContentSize == ZSTD_CONTENTSIZE_ERROR
                  || frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN ) {
                    info->decompUnavailable = 1;
                } else {
                    info->decompressedSize += frameContentSize;
                }
                ERROR_IF(ZSTD_getFrameHeader(&header, headerBuffer, numBytesRead) != 0,
                        info_frame_error, "Error: could not decode frame header");
                if (info->dictID != 0 && info->dictID != header.dictID) {
                    DISPLAY("WARNING: File contains multiple frames with different dictionary IDs. Showing dictID 0 instead");
                    info->dictID = 0;
                } else {
                    info->dictID = header.dictID;
                }
                info->windowSize = header.windowSize;
                /* move to the end of the frame header */
                {   size_t const headerSize = ZSTD_frameHeaderSize(headerBuffer, numBytesRead);
                    ERROR_IF(ZSTD_isError(headerSize), info_frame_error, "Error: could not determine frame header size");
                    ERROR_IF(fseek(srcFile, ((long)headerSize)-((long)numBytesRead), SEEK_CUR) != 0,
                            info_frame_error, "Error: could not move to end of frame header");
                }

                /* skip all blocks in the frame */
                {   int lastBlock = 0;
                    do {
                        BYTE blockHeaderBuffer[3];
                        ERROR_IF(fread(blockHeaderBuffer, 1, 3, srcFile) != 3,
                                info_frame_error, "Error while reading block header");
                        {   U32 const blockHeader = MEM_readLE24(blockHeaderBuffer);
                            U32 const blockTypeID = (blockHeader >> 1) & 3;
                            U32 const isRLE = (blockTypeID == 1);
                            U32 const isWrongBlock = (blockTypeID == 3);
                            long const blockSize = isRLE ? 1 : (long)(blockHeader >> 3);
                            ERROR_IF(isWrongBlock, info_frame_error, "Error: unsupported block type");
                            lastBlock = blockHeader & 1;
                            ERROR_IF(fseek(srcFile, blockSize, SEEK_CUR) != 0,
                                    info_frame_error, "Error: could not skip to end of block");
                        }
                    } while (lastBlock != 1);
                }

                /* check if checksum is used */
                {   BYTE const frameHeaderDescriptor = headerBuffer[4];
                    int const contentChecksumFlag = (frameHeaderDescriptor & (1 << 2)) >> 2;
                    if (contentChecksumFlag) {
                        info->usesCheck = 1;
                        ERROR_IF(fread(info->checksum, 1, 4, srcFile) != 4,
                                info_frame_error, "Error: could not read checksum");
                }   }
                info->numActualFrames++;
            }
            /* Skippable frame */
            else if ((magicNumber & ZSTD_MAGIC_SKIPPABLE_MASK) == ZSTD_MAGIC_SKIPPABLE_START) {
                U32 const frameSize = MEM_readLE32(headerBuffer + 4);
                long const seek = (long)(8 + frameSize - numBytesRead);
                ERROR_IF(LONG_SEEK(srcFile, seek, SEEK_CUR) != 0,
                        info_frame_error, "Error: could not find end of skippable frame");
                info->numSkippableFrames++;
            }
            /* unknown content */
            else {
                return info_not_zstd;
            }
        }  /* magic number analysis */
    }  /* end analyzing frames */
    return info_success;
}


static InfoError
getFileInfo_fileConfirmed(fileInfo_t* info, const char* inFileName)
{
    InfoError status;
    stat_t srcFileStat;
    FILE* const srcFile = FIO_openSrcFile(NULL, inFileName, &srcFileStat);
    ERROR_IF(srcFile == NULL, info_file_error, "Error: could not open source file %s", inFileName);

    info->compressedSize = UTIL_getFileSizeStat(&srcFileStat);
    status = FIO_analyzeFrames(info, srcFile);

    fclose(srcFile);
    info->nbFiles = 1;
    return status;
}


/** getFileInfo() :
 *  Reads information from file, stores in *info
 * @return : InfoError status
 */
static InfoError
getFileInfo(fileInfo_t* info, const char* srcFileName)
{
    ERROR_IF(!UTIL_isRegularFile(srcFileName),
            info_file_error, "Error : %s is not a file", srcFileName);
    return getFileInfo_fileConfirmed(info, srcFileName);
}


static void
displayInfo(const char* inFileName, const fileInfo_t* info, int displayLevel)
{
    UTIL_HumanReadableSize_t const window_hrs = UTIL_makeHumanReadableSize(info->windowSize);
    UTIL_HumanReadableSize_t const compressed_hrs = UTIL_makeHumanReadableSize(info->compressedSize);
    UTIL_HumanReadableSize_t const decompressed_hrs = UTIL_makeHumanReadableSize(info->decompressedSize);
    double const ratio = (info->compressedSize == 0) ? 0 : ((double)info->decompressedSize)/(double)info->compressedSize;
    const char* const checkString = (info->usesCheck ? "XXH64" : "None");
    if (displayLevel <= 2) {
        if (!info->decompUnavailable) {
            DISPLAYOUT("%6d  %5d  %6.*f%4s  %8.*f%4s  %5.3f  %5s  %s\n",
                    info->numSkippableFrames + info->numActualFrames,
                    info->numSkippableFrames,
                    compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                    decompressed_hrs.precision, decompressed_hrs.value, decompressed_hrs.suffix,
                    ratio, checkString, inFileName);
        } else {
            DISPLAYOUT("%6d  %5d  %6.*f%4s                       %5s  %s\n",
                    info->numSkippableFrames + info->numActualFrames,
                    info->numSkippableFrames,
                    compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                    checkString, inFileName);
        }
    } else {
        DISPLAYOUT("%s \n", inFileName);
        DISPLAYOUT("# Zstandard Frames: %d\n", info->numActualFrames);
        if (info->numSkippableFrames)
            DISPLAYOUT("# Skippable Frames: %d\n", info->numSkippableFrames);
        DISPLAYOUT("DictID: %u\n", info->dictID);
        DISPLAYOUT("Window Size: %.*f%s (%llu B)\n",
                   window_hrs.precision, window_hrs.value, window_hrs.suffix,
                   (unsigned long long)info->windowSize);
        DISPLAYOUT("Compressed Size: %.*f%s (%llu B)\n",
                    compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                    (unsigned long long)info->compressedSize);
        if (!info->decompUnavailable) {
            DISPLAYOUT("Decompressed Size: %.*f%s (%llu B)\n",
                    decompressed_hrs.precision, decompressed_hrs.value, decompressed_hrs.suffix,
                    (unsigned long long)info->decompressedSize);
            DISPLAYOUT("Ratio: %.4f\n", ratio);
        }

        if (info->usesCheck && info->numActualFrames == 1) {
            DISPLAYOUT("Check: %s %02x%02x%02x%02x\n", checkString,
                info->checksum[3], info->checksum[2],
                info->checksum[1], info->checksum[0]
            );
        } else {
            DISPLAYOUT("Check: %s\n", checkString);
        }

        DISPLAYOUT("\n");
    }
}

static fileInfo_t FIO_addFInfo(fileInfo_t fi1, fileInfo_t fi2)
{
    fileInfo_t total;
    memset(&total, 0, sizeof(total));
    total.numActualFrames = fi1.numActualFrames + fi2.numActualFrames;
    total.numSkippableFrames = fi1.numSkippableFrames + fi2.numSkippableFrames;
    total.compressedSize = fi1.compressedSize + fi2.compressedSize;
    total.decompressedSize = fi1.decompressedSize + fi2.decompressedSize;
    total.decompUnavailable = fi1.decompUnavailable | fi2.decompUnavailable;
    total.usesCheck = fi1.usesCheck & fi2.usesCheck;
    total.nbFiles = fi1.nbFiles + fi2.nbFiles;
    return total;
}

static int
FIO_listFile(fileInfo_t* total, const char* inFileName, int displayLevel)
{
    fileInfo_t info;
    memset(&info, 0, sizeof(info));
    {   InfoError const error = getFileInfo(&info, inFileName);
        switch (error) {
            case info_frame_error:
                /* display error, but provide output */
                DISPLAYLEVEL(1, "Error while parsing \"%s\" \n", inFileName);
                break;
            case info_not_zstd:
                DISPLAYOUT("File \"%s\" not compressed by zstd \n", inFileName);
                if (displayLevel > 2) DISPLAYOUT("\n");
                return 1;
            case info_file_error:
                /* error occurred while opening the file */
                if (displayLevel > 2) DISPLAYOUT("\n");
                return 1;
            case info_truncated_input:
                DISPLAYOUT("File \"%s\" is truncated \n", inFileName);
                if (displayLevel > 2) DISPLAYOUT("\n");
                return 1;
            case info_success:
            default:
                break;
        }

        displayInfo(inFileName, &info, displayLevel);
        *total = FIO_addFInfo(*total, info);
        assert(error == info_success || error == info_frame_error);
        return (int)error;
    }
}

int FIO_listMultipleFiles(unsigned numFiles, const char** filenameTable, int displayLevel)
{
    /* ensure no specified input is stdin (needs fseek() capability) */
    {   unsigned u;
        for (u=0; u<numFiles;u++) {
            ERROR_IF(!strcmp (filenameTable[u], stdinmark),
                    1, "zstd: --list does not support reading from standard input");
    }   }

    if (numFiles == 0) {
        if (!UTIL_isConsole(stdin)) {
            DISPLAYLEVEL(1, "zstd: --list does not support reading from standard input \n");
        }
        DISPLAYLEVEL(1, "No files given \n");
        return 1;
    }

    if (displayLevel <= 2) {
        DISPLAYOUT("Frames  Skips  Compressed  Uncompressed  Ratio  Check  Filename\n");
    }
    {   int error = 0;
        fileInfo_t total;
        memset(&total, 0, sizeof(total));
        total.usesCheck = 1;
        /* --list each file, and check for any error */
        {   unsigned u;
            for (u=0; u<numFiles;u++) {
                error |= FIO_listFile(&total, filenameTable[u], displayLevel);
        }   }
        if (numFiles > 1 && displayLevel <= 2) {   /* display total */
            UTIL_HumanReadableSize_t const compressed_hrs = UTIL_makeHumanReadableSize(total.compressedSize);
            UTIL_HumanReadableSize_t const decompressed_hrs = UTIL_makeHumanReadableSize(total.decompressedSize);
            double const ratio = (total.compressedSize == 0) ? 0 : ((double)total.decompressedSize)/(double)total.compressedSize;
            const char* const checkString = (total.usesCheck ? "XXH64" : "");
            DISPLAYOUT("----------------------------------------------------------------- \n");
            if (total.decompUnavailable) {
                DISPLAYOUT("%6d  %5d  %6.*f%4s                       %5s  %u files\n",
                        total.numSkippableFrames + total.numActualFrames,
                        total.numSkippableFrames,
                        compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                        checkString, (unsigned)total.nbFiles);
            } else {
                DISPLAYOUT("%6d  %5d  %6.*f%4s  %8.*f%4s  %5.3f  %5s  %u files\n",
                        total.numSkippableFrames + total.numActualFrames,
                        total.numSkippableFrames,
                        compressed_hrs.precision, compressed_hrs.value, compressed_hrs.suffix,
                        decompressed_hrs.precision, decompressed_hrs.value, decompressed_hrs.suffix,
                        ratio, checkString, (unsigned)total.nbFiles);
        }   }
        return error;
    }
}


#endif /* #ifndef ZSTD_NODECOMPRESS */
