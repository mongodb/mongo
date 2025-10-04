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
*  Tuning parameters
**************************************/
#ifndef ZSTDCLI_CLEVEL_DEFAULT
#  define ZSTDCLI_CLEVEL_DEFAULT 3
#endif

#ifndef ZSTDCLI_CLEVEL_MAX
#  define ZSTDCLI_CLEVEL_MAX 19   /* without using --ultra */
#endif

#ifndef ZSTDCLI_NBTHREADS_DEFAULT
#  define ZSTDCLI_NBTHREADS_DEFAULT 1
#endif

/*-************************************
*  Dependencies
**************************************/
#include "platform.h" /* PLATFORM_POSIX_VERSION */
#include "util.h"     /* UTIL_HAS_CREATEFILELIST, UTIL_createFileList, UTIL_isConsole */
#include <stdlib.h>   /* getenv */
#include <string.h>   /* strcmp, strlen */
#include <stdio.h>    /* fprintf(), stdin, stdout, stderr */
#include <errno.h>    /* errno */
#include <assert.h>   /* assert */

#include "fileio.h"   /* stdinmark, stdoutmark, ZSTD_EXTENSION */
#ifndef ZSTD_NOBENCH
#  include "benchzstd.h"  /* BMK_benchFilesAdvanced */
#endif
#ifndef ZSTD_NODICT
#  include "dibio.h"  /* ZDICT_cover_params_t, DiB_trainFromFiles() */
#endif
#ifndef ZSTD_NOTRACE
#  include "zstdcli_trace.h"
#endif
#include "../lib/zstd.h"  /* ZSTD_VERSION_STRING, ZSTD_minCLevel, ZSTD_maxCLevel */
#include "fileio_asyncio.h"


/*-************************************
*  Constants
**************************************/
#define COMPRESSOR_NAME "Zstandard CLI"
#ifndef ZSTD_VERSION
#  define ZSTD_VERSION "v" ZSTD_VERSION_STRING
#endif
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s (%i-bit) %s, by %s ***\n", COMPRESSOR_NAME, (int)(sizeof(size_t)*8), ZSTD_VERSION, AUTHOR

#define ZSTD_ZSTDMT "zstdmt"
#define ZSTD_UNZSTD "unzstd"
#define ZSTD_CAT "zstdcat"
#define ZSTD_ZCAT "zcat"
#define ZSTD_GZ "gzip"
#define ZSTD_GUNZIP "gunzip"
#define ZSTD_GZCAT "gzcat"
#define ZSTD_LZMA "lzma"
#define ZSTD_UNLZMA "unlzma"
#define ZSTD_XZ "xz"
#define ZSTD_UNXZ "unxz"
#define ZSTD_LZ4 "lz4"
#define ZSTD_UNLZ4 "unlz4"

#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)

#define DISPLAY_LEVEL_DEFAULT 2

static const char*    g_defaultDictName = "dictionary";
static const unsigned g_defaultMaxDictSize = 110 KB;
static const int      g_defaultDictCLevel = 3;
static const unsigned g_defaultSelectivityLevel = 9;
static const unsigned g_defaultMaxWindowLog = 27;
#define OVERLAP_LOG_DEFAULT 9999
#define LDM_PARAM_DEFAULT 9999  /* Default for parameters where 0 is valid */
static U32 g_overlapLog = OVERLAP_LOG_DEFAULT;
static U32 g_ldmHashLog = 0;
static U32 g_ldmMinMatch = 0;
static U32 g_ldmHashRateLog = LDM_PARAM_DEFAULT;
static U32 g_ldmBucketSizeLog = LDM_PARAM_DEFAULT;


#define DEFAULT_ACCEL 1

typedef enum { cover, fastCover, legacy } dictType;

/*-************************************
*  Display Macros
**************************************/
#define DISPLAY_F(f, ...)    fprintf((f), __VA_ARGS__)
#define DISPLAYOUT(...)      DISPLAY_F(stdout, __VA_ARGS__)
#define DISPLAY(...)         DISPLAY_F(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) { if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); } }
static int g_displayLevel = DISPLAY_LEVEL_DEFAULT;   /* 0 : no display,  1: errors,  2 : + result + interaction + warnings,  3 : + progression,  4 : + information */


/*-************************************
*  Check Version (when CLI linked to dynamic library)
**************************************/

/* Due to usage of experimental symbols and capabilities by the CLI,
 * the CLI must be linked against a dynamic library of same version */
static void checkLibVersion(void)
{
    if (strcmp(ZSTD_VERSION_STRING, ZSTD_versionString())) {
        DISPLAYLEVEL(1, "Error : incorrect library version (expecting : %s ; actual : %s ) \n",
                    ZSTD_VERSION_STRING, ZSTD_versionString());
        DISPLAYLEVEL(1, "Please update library to version %s, or use stand-alone zstd binary \n",
                    ZSTD_VERSION_STRING);
        exit(1);
    }
}


/*! exeNameMatch() :
    @return : a non-zero value if exeName matches test, excluding the extension
   */
static int exeNameMatch(const char* exeName, const char* test)
{
    return !strncmp(exeName, test, strlen(test)) &&
        (exeName[strlen(test)] == '\0' || exeName[strlen(test)] == '.');
}

/*-************************************
*  Command Line
**************************************/
/* print help either in `stderr` or `stdout` depending on originating request
 * error (badusage) => stderr
 * help (usage_advanced) => stdout
 */
static void usage(FILE* f, const char* programName)
{
    DISPLAY_F(f, "Compress or decompress the INPUT file(s); reads from STDIN if INPUT is `-` or not provided.\n\n");
    DISPLAY_F(f, "Usage: %s [OPTIONS...] [INPUT... | -] [-o OUTPUT]\n\n", programName);
    DISPLAY_F(f, "Options:\n");
    DISPLAY_F(f, "  -o OUTPUT                     Write output to a single file, OUTPUT.\n");
    DISPLAY_F(f, "  -k, --keep                    Preserve INPUT file(s). [Default] \n");
    DISPLAY_F(f, "  --rm                          Remove INPUT file(s) after successful (de)compression.\n");
#ifdef ZSTD_GZCOMPRESS
    if (exeNameMatch(programName, ZSTD_GZ)) {     /* behave like gzip */
        DISPLAY_F(f, "  -n, --no-name                 Do not store original filename when compressing.\n\n");
    }
#endif
    DISPLAY_F(f, "\n");
#ifndef ZSTD_NOCOMPRESS
    DISPLAY_F(f, "  -#                            Desired compression level, where `#` is a number between 1 and %d;\n", ZSTDCLI_CLEVEL_MAX);
    DISPLAY_F(f, "                                lower numbers provide faster compression, higher numbers yield\n");
    DISPLAY_F(f, "                                better compression ratios. [Default: %d]\n\n", ZSTDCLI_CLEVEL_DEFAULT);
#endif
#ifndef ZSTD_NODECOMPRESS
    DISPLAY_F(f, "  -d, --decompress              Perform decompression.\n");
#endif
    DISPLAY_F(f, "  -D DICT                       Use DICT as the dictionary for compression or decompression.\n\n");
    DISPLAY_F(f, "  -f, --force                   Disable input and output checks. Allows overwriting existing files,\n");
    DISPLAY_F(f, "                                receiving input from the console, printing output to STDOUT, and\n");
    DISPLAY_F(f, "                                operating on links, block devices, etc. Unrecognized formats will be\n");
    DISPLAY_F(f, "                                passed-through through as-is.\n\n");

    DISPLAY_F(f, "  -h                            Display short usage and exit.\n");
    DISPLAY_F(f, "  -H, --help                    Display full help and exit.\n");
    DISPLAY_F(f, "  -V, --version                 Display the program version and exit.\n");
    DISPLAY_F(f, "\n");
}

static void usage_advanced(const char* programName)
{
    DISPLAYOUT(WELCOME_MESSAGE);
    DISPLAYOUT("\n");
    usage(stdout, programName);
    DISPLAYOUT("Advanced options:\n");
    DISPLAYOUT("  -c, --stdout                  Write to STDOUT (even if it is a console) and keep the INPUT file(s).\n\n");

    DISPLAYOUT("  -v, --verbose                 Enable verbose output; pass multiple times to increase verbosity.\n");
    DISPLAYOUT("  -q, --quiet                   Suppress warnings; pass twice to suppress errors.\n");
#ifndef ZSTD_NOTRACE
    DISPLAYOUT("  --trace LOG                   Log tracing information to LOG.\n");
#endif
    DISPLAYOUT("\n");
    DISPLAYOUT("  --[no-]progress               Forcibly show/hide the progress counter. NOTE: Any (de)compressed\n");
    DISPLAYOUT("                                output to terminal will mix with progress counter text.\n\n");

#ifdef UTIL_HAS_CREATEFILELIST
    DISPLAYOUT("  -r                            Operate recursively on directories.\n");
    DISPLAYOUT("  --filelist LIST               Read a list of files to operate on from LIST.\n");
    DISPLAYOUT("  --output-dir-flat DIR         Store processed files in DIR.\n");
#endif

#ifdef UTIL_HAS_MIRRORFILELIST
    DISPLAYOUT("  --output-dir-mirror DIR       Store processed files in DIR, respecting original directory structure.\n");
#endif
    if (AIO_supported())
        DISPLAYOUT("  --[no-]asyncio                Use asynchronous IO. [Default: Enabled]\n");

    DISPLAYOUT("\n");
#ifndef ZSTD_NOCOMPRESS
    DISPLAYOUT("  --[no-]check                  Add XXH64 integrity checksums during compression. [Default: Add, Validate]\n");
#ifndef ZSTD_NODECOMPRESS
    DISPLAYOUT("                                If `-d` is present, ignore/validate checksums during decompression.\n");
#endif
#else
#ifdef ZSTD_NOCOMPRESS
    DISPLAYOUT("  --[no-]check                  Ignore/validate checksums during decompression. [Default: Validate]");
#endif
#endif /* ZSTD_NOCOMPRESS */

    DISPLAYOUT("\n");
    DISPLAYOUT("  --                            Treat remaining arguments after `--` as files.\n");

#ifndef ZSTD_NOCOMPRESS
    DISPLAYOUT("\n");
    DISPLAYOUT("Advanced compression options:\n");
    DISPLAYOUT("  --ultra                       Enable levels beyond %i, up to %i; requires more memory.\n", ZSTDCLI_CLEVEL_MAX, ZSTD_maxCLevel());
    DISPLAYOUT("  --fast[=#]                    Use to very fast compression levels. [Default: %u]\n", 1);
#ifdef ZSTD_GZCOMPRESS
    if (exeNameMatch(programName, ZSTD_GZ)) {     /* behave like gzip */
        DISPLAYOUT("  --best                        Compatibility alias for `-9`.\n");
    }
#endif
    DISPLAYOUT("  --adapt                       Dynamically adapt compression level to I/O conditions.\n");
    DISPLAYOUT("  --long[=#]                    Enable long distance matching with window log #. [Default: %u]\n", g_defaultMaxWindowLog);
    DISPLAYOUT("  --patch-from=REF              Use REF as the reference point for Zstandard's diff engine. \n\n");
# ifdef ZSTD_MULTITHREAD
    DISPLAYOUT("  -T#                           Spawn # compression threads. [Default: 1; pass 0 for core count.]\n");
    DISPLAYOUT("  --single-thread               Share a single thread for I/O and compression (slightly different than `-T1`).\n");
    DISPLAYOUT("  --auto-threads={physical|logical}\n");
    DISPLAYOUT("                                Use physical/logical cores when using `-T0`. [Default: Physical]\n\n");
    DISPLAYOUT("  -B#                           Set job size to #. [Default: 0 (automatic)]\n");
    DISPLAYOUT("  --rsyncable                   Compress using a rsync-friendly method (`-B` sets block size). \n");
    DISPLAYOUT("\n");
# endif
    DISPLAYOUT("  --exclude-compressed          Only compress files that are not already compressed.\n\n");

    DISPLAYOUT("  --stream-size=#               Specify size of streaming input from STDIN.\n");
    DISPLAYOUT("  --size-hint=#                 Optimize compression parameters for streaming input of approximately size #.\n");
    DISPLAYOUT("  --target-compressed-block-size=#\n");
    DISPLAYOUT("                                Generate compressed blocks of approximately # size.\n\n");
    DISPLAYOUT("  --no-dictID                   Don't write `dictID` into the header (dictionary compression only).\n");
    DISPLAYOUT("  --[no-]compress-literals      Force (un)compressed literals.\n");
    DISPLAYOUT("  --[no-]row-match-finder       Explicitly enable/disable the fast, row-based matchfinder for\n");
    DISPLAYOUT("                                the 'greedy', 'lazy', and 'lazy2' strategies.\n");

    DISPLAYOUT("\n");
    DISPLAYOUT("  --format=zstd                 Compress files to the `.zst` format. [Default]\n");
    DISPLAYOUT("  --mmap-dict                   Memory-map dictionary file rather than mallocing and loading all at once");
#ifdef ZSTD_GZCOMPRESS
    DISPLAYOUT("  --format=gzip                 Compress files to the `.gz` format.\n");
#endif
#ifdef ZSTD_LZMACOMPRESS
    DISPLAYOUT("  --format=xz                   Compress files to the `.xz` format.\n");
    DISPLAYOUT("  --format=lzma                 Compress files to the `.lzma` format.\n");
#endif
#ifdef ZSTD_LZ4COMPRESS
    DISPLAYOUT( "  --format=lz4                 Compress files to the `.lz4` format.\n");
#endif
#endif  /* !ZSTD_NOCOMPRESS */

#ifndef ZSTD_NODECOMPRESS
    DISPLAYOUT("\n");
    DISPLAYOUT("Advanced decompression options:\n");
    DISPLAYOUT("  -l                            Print information about Zstandard-compressed files.\n");
    DISPLAYOUT("  --test                        Test compressed file integrity.\n");
    DISPLAYOUT("  -M#                           Set the memory usage limit to # megabytes.\n");
# if ZSTD_SPARSE_DEFAULT
    DISPLAYOUT("  --[no-]sparse                 Enable sparse mode. [Default: Enabled for files, disabled for STDOUT.]\n");
# else
    DISPLAYOUT("  --[no-]sparse                 Enable sparse mode. [Default: Disabled]\n");
# endif
    {
        char const* passThroughDefault = "Disabled";
        if (exeNameMatch(programName, ZSTD_CAT) ||
            exeNameMatch(programName, ZSTD_ZCAT) ||
            exeNameMatch(programName, ZSTD_GZCAT)) {
            passThroughDefault = "Enabled";
        }
        DISPLAYOUT("  --[no-]pass-through           Pass through uncompressed files as-is. [Default: %s]\n", passThroughDefault);
    }
#endif  /* ZSTD_NODECOMPRESS */

#ifndef ZSTD_NODICT
    DISPLAYOUT("\n");
    DISPLAYOUT("Dictionary builder:\n");
    DISPLAYOUT("  --train                       Create a dictionary from a training set of files.\n\n");
    DISPLAYOUT("  --train-cover[=k=#,d=#,steps=#,split=#,shrink[=#]]\n");
    DISPLAYOUT("                                Use the cover algorithm (with optional arguments).\n");
    DISPLAYOUT("  --train-fastcover[=k=#,d=#,f=#,steps=#,split=#,accel=#,shrink[=#]]\n");
    DISPLAYOUT("                                Use the fast cover algorithm (with optional arguments).\n\n");
    DISPLAYOUT("  --train-legacy[=s=#]          Use the legacy algorithm with selectivity #. [Default: %u]\n", g_defaultSelectivityLevel);
    DISPLAYOUT("  -o NAME                       Use NAME as dictionary name. [Default: %s]\n", g_defaultDictName);
    DISPLAYOUT("  --maxdict=#                   Limit dictionary to specified size #. [Default: %u]\n", g_defaultMaxDictSize);
    DISPLAYOUT("  --dictID=#                    Force dictionary ID to #. [Default: Random]\n");
#endif

#ifndef ZSTD_NOBENCH
    DISPLAYOUT("\n");
    DISPLAYOUT("Benchmark options:\n");
    DISPLAYOUT("  -b#                           Perform benchmarking with compression level #. [Default: %d]\n", ZSTDCLI_CLEVEL_DEFAULT);
    DISPLAYOUT("  -e#                           Test all compression levels up to #; starting level is `-b#`. [Default: 1]\n");
    DISPLAYOUT("  -i#                           Set the minimum evaluation to time # seconds. [Default: 3]\n");
    DISPLAYOUT("  -B#                           Cut file into independent chunks of size #. [Default: No chunking]\n");
    DISPLAYOUT("  -S                            Output one benchmark result per input file. [Default: Consolidated result]\n");
    DISPLAYOUT("  --priority=rt                 Set process priority to real-time.\n");
#endif

}

static void badusage(const char* programName)
{
    DISPLAYLEVEL(1, "Incorrect parameters \n");
    if (g_displayLevel >= 2) usage(stderr, programName);
}

static void waitEnter(void)
{
    int unused;
    DISPLAY("Press enter to continue... \n");
    unused = getchar();
    (void)unused;
}

static const char* lastNameFromPath(const char* path)
{
    const char* name = path;
    if (strrchr(name, '/')) name = strrchr(name, '/') + 1;
    if (strrchr(name, '\\')) name = strrchr(name, '\\') + 1; /* windows */
    return name;
}

static void errorOut(const char* msg)
{
    DISPLAYLEVEL(1, "%s \n", msg); exit(1);
}

/*! readU32FromCharChecked() :
 * @return 0 if success, and store the result in *value.
 *  allows and interprets K, KB, KiB, M, MB and MiB suffix.
 *  Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 * @return 1 if an overflow error occurs */
static int readU32FromCharChecked(const char** stringPtr, unsigned* value)
{
    unsigned result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9')) {
        unsigned const max = ((unsigned)(-1)) / 10;
        unsigned last = result;
        if (result > max) return 1; /* overflow error */
        result *= 10;
        result += (unsigned)(**stringPtr - '0');
        if (result < last) return 1; /* overflow error */
        (*stringPtr)++ ;
    }
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        unsigned const maxK = ((unsigned)(-1)) >> 10;
        if (result > maxK) return 1; /* overflow error */
        result <<= 10;
        if (**stringPtr=='M') {
            if (result > maxK) return 1; /* overflow error */
            result <<= 10;
        }
        (*stringPtr)++;  /* skip `K` or `M` */
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    *value = result;
    return 0;
}

/*! readU32FromChar() :
 * @return : unsigned integer value read from input in `char` format.
 *  allows and interprets K, KB, KiB, M, MB and MiB suffix.
 *  Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 *  Note : function will exit() program if digit sequence overflows */
static unsigned readU32FromChar(const char** stringPtr) {
    static const char errorMsg[] = "error: numeric value overflows 32-bit unsigned int";
    unsigned result;
    if (readU32FromCharChecked(stringPtr, &result)) { errorOut(errorMsg); }
    return result;
}

/*! readIntFromChar() :
 * @return : signed integer value read from input in `char` format.
 *  allows and interprets K, KB, KiB, M, MB and MiB suffix.
 *  Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 *  Note : function will exit() program if digit sequence overflows */
static int readIntFromChar(const char** stringPtr) {
    static const char errorMsg[] = "error: numeric value overflows 32-bit int";
    int sign = 1;
    unsigned result;
    if (**stringPtr=='-') {
        (*stringPtr)++;
        sign = -1;
    }
    if (readU32FromCharChecked(stringPtr, &result)) { errorOut(errorMsg); }
    return (int) result * sign;
}

/*! readSizeTFromCharChecked() :
 * @return 0 if success, and store the result in *value.
 *  allows and interprets K, KB, KiB, M, MB and MiB suffix.
 *  Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 * @return 1 if an overflow error occurs */
static int readSizeTFromCharChecked(const char** stringPtr, size_t* value)
{
    size_t result = 0;
    while ((**stringPtr >='0') && (**stringPtr <='9')) {
        size_t const max = ((size_t)(-1)) / 10;
        size_t last = result;
        if (result > max) return 1; /* overflow error */
        result *= 10;
        result += (size_t)(**stringPtr - '0');
        if (result < last) return 1; /* overflow error */
        (*stringPtr)++ ;
    }
    if ((**stringPtr=='K') || (**stringPtr=='M')) {
        size_t const maxK = ((size_t)(-1)) >> 10;
        if (result > maxK) return 1; /* overflow error */
        result <<= 10;
        if (**stringPtr=='M') {
            if (result > maxK) return 1; /* overflow error */
            result <<= 10;
        }
        (*stringPtr)++;  /* skip `K` or `M` */
        if (**stringPtr=='i') (*stringPtr)++;
        if (**stringPtr=='B') (*stringPtr)++;
    }
    *value = result;
    return 0;
}

/*! readSizeTFromChar() :
 * @return : size_t value read from input in `char` format.
 *  allows and interprets K, KB, KiB, M, MB and MiB suffix.
 *  Will also modify `*stringPtr`, advancing it to position where it stopped reading.
 *  Note : function will exit() program if digit sequence overflows */
static size_t readSizeTFromChar(const char** stringPtr) {
    static const char errorMsg[] = "error: numeric value overflows size_t";
    size_t result;
    if (readSizeTFromCharChecked(stringPtr, &result)) { errorOut(errorMsg); }
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


#ifndef ZSTD_NODICT

static const unsigned kDefaultRegression = 1;
/**
 * parseCoverParameters() :
 * reads cover parameters from *stringPtr (e.g. "--train-cover=k=48,d=8,steps=32") into *params
 * @return 1 means that cover parameters were correct
 * @return 0 in case of malformed parameters
 */
static unsigned parseCoverParameters(const char* stringPtr, ZDICT_cover_params_t* params)
{
    memset(params, 0, sizeof(*params));
    for (; ;) {
        if (longCommandWArg(&stringPtr, "k=")) { params->k = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "d=")) { params->d = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "steps=")) { params->steps = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "split=")) {
          unsigned splitPercentage = readU32FromChar(&stringPtr);
          params->splitPoint = (double)splitPercentage / 100.0;
          if (stringPtr[0]==',') { stringPtr++; continue; } else break;
        }
        if (longCommandWArg(&stringPtr, "shrink")) {
          params->shrinkDictMaxRegression = kDefaultRegression;
          params->shrinkDict = 1;
          if (stringPtr[0]=='=') {
            stringPtr++;
            params->shrinkDictMaxRegression = readU32FromChar(&stringPtr);
          }
          if (stringPtr[0]==',') {
            stringPtr++;
            continue;
          }
          else break;
        }
        return 0;
    }
    if (stringPtr[0] != 0) return 0;
    DISPLAYLEVEL(4, "cover: k=%u\nd=%u\nsteps=%u\nsplit=%u\nshrink%u\n", params->k, params->d, params->steps, (unsigned)(params->splitPoint * 100), params->shrinkDictMaxRegression);
    return 1;
}

/**
 * parseFastCoverParameters() :
 * reads fastcover parameters from *stringPtr (e.g. "--train-fastcover=k=48,d=8,f=20,steps=32,accel=2") into *params
 * @return 1 means that fastcover parameters were correct
 * @return 0 in case of malformed parameters
 */
static unsigned parseFastCoverParameters(const char* stringPtr, ZDICT_fastCover_params_t* params)
{
    memset(params, 0, sizeof(*params));
    for (; ;) {
        if (longCommandWArg(&stringPtr, "k=")) { params->k = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "d=")) { params->d = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "f=")) { params->f = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "steps=")) { params->steps = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "accel=")) { params->accel = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "split=")) {
          unsigned splitPercentage = readU32FromChar(&stringPtr);
          params->splitPoint = (double)splitPercentage / 100.0;
          if (stringPtr[0]==',') { stringPtr++; continue; } else break;
        }
        if (longCommandWArg(&stringPtr, "shrink")) {
          params->shrinkDictMaxRegression = kDefaultRegression;
          params->shrinkDict = 1;
          if (stringPtr[0]=='=') {
            stringPtr++;
            params->shrinkDictMaxRegression = readU32FromChar(&stringPtr);
          }
          if (stringPtr[0]==',') {
            stringPtr++;
            continue;
          }
          else break;
        }
        return 0;
    }
    if (stringPtr[0] != 0) return 0;
    DISPLAYLEVEL(4, "cover: k=%u\nd=%u\nf=%u\nsteps=%u\nsplit=%u\naccel=%u\nshrink=%u\n", params->k, params->d, params->f, params->steps, (unsigned)(params->splitPoint * 100), params->accel, params->shrinkDictMaxRegression);
    return 1;
}

/**
 * parseLegacyParameters() :
 * reads legacy dictionary builder parameters from *stringPtr (e.g. "--train-legacy=selectivity=8") into *selectivity
 * @return 1 means that legacy dictionary builder parameters were correct
 * @return 0 in case of malformed parameters
 */
static unsigned parseLegacyParameters(const char* stringPtr, unsigned* selectivity)
{
    if (!longCommandWArg(&stringPtr, "s=") && !longCommandWArg(&stringPtr, "selectivity=")) { return 0; }
    *selectivity = readU32FromChar(&stringPtr);
    if (stringPtr[0] != 0) return 0;
    DISPLAYLEVEL(4, "legacy: selectivity=%u\n", *selectivity);
    return 1;
}

static ZDICT_cover_params_t defaultCoverParams(void)
{
    ZDICT_cover_params_t params;
    memset(&params, 0, sizeof(params));
    params.d = 8;
    params.steps = 4;
    params.splitPoint = 1.0;
    params.shrinkDict = 0;
    params.shrinkDictMaxRegression = kDefaultRegression;
    return params;
}

static ZDICT_fastCover_params_t defaultFastCoverParams(void)
{
    ZDICT_fastCover_params_t params;
    memset(&params, 0, sizeof(params));
    params.d = 8;
    params.f = 20;
    params.steps = 4;
    params.splitPoint = 0.75; /* different from default splitPoint of cover */
    params.accel = DEFAULT_ACCEL;
    params.shrinkDict = 0;
    params.shrinkDictMaxRegression = kDefaultRegression;
    return params;
}
#endif


/** parseAdaptParameters() :
 *  reads adapt parameters from *stringPtr (e.g. "--zstd=min=1,max=19) and store them into adaptMinPtr and adaptMaxPtr.
 *  Both adaptMinPtr and adaptMaxPtr must be already allocated and correctly initialized.
 *  There is no guarantee that any of these values will be updated.
 *  @return 1 means that parsing was successful,
 *  @return 0 in case of malformed parameters
 */
static unsigned parseAdaptParameters(const char* stringPtr, int* adaptMinPtr, int* adaptMaxPtr)
{
    for ( ; ;) {
        if (longCommandWArg(&stringPtr, "min=")) { *adaptMinPtr = readIntFromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "max=")) { *adaptMaxPtr = readIntFromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        DISPLAYLEVEL(4, "invalid compression parameter \n");
        return 0;
    }
    if (stringPtr[0] != 0) return 0; /* check the end of string */
    if (*adaptMinPtr > *adaptMaxPtr) {
        DISPLAYLEVEL(4, "incoherent adaptation limits \n");
        return 0;
    }
    return 1;
}


/** parseCompressionParameters() :
 *  reads compression parameters from *stringPtr (e.g. "--zstd=wlog=23,clog=23,hlog=22,slog=6,mml=3,tlen=48,strat=6") into *params
 *  @return 1 means that compression parameters were correct
 *  @return 0 in case of malformed parameters
 */
static unsigned parseCompressionParameters(const char* stringPtr, ZSTD_compressionParameters* params)
{
    for ( ; ;) {
        if (longCommandWArg(&stringPtr, "windowLog=") || longCommandWArg(&stringPtr, "wlog=")) { params->windowLog = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "chainLog=") || longCommandWArg(&stringPtr, "clog=")) { params->chainLog = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "hashLog=") || longCommandWArg(&stringPtr, "hlog=")) { params->hashLog = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "searchLog=") || longCommandWArg(&stringPtr, "slog=")) { params->searchLog = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "minMatch=") || longCommandWArg(&stringPtr, "mml=")) { params->minMatch = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "targetLength=") || longCommandWArg(&stringPtr, "tlen=")) { params->targetLength = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "strategy=") || longCommandWArg(&stringPtr, "strat=")) { params->strategy = (ZSTD_strategy)(readU32FromChar(&stringPtr)); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "overlapLog=") || longCommandWArg(&stringPtr, "ovlog=")) { g_overlapLog = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "ldmHashLog=") || longCommandWArg(&stringPtr, "lhlog=")) { g_ldmHashLog = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "ldmMinMatch=") || longCommandWArg(&stringPtr, "lmml=")) { g_ldmMinMatch = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "ldmBucketSizeLog=") || longCommandWArg(&stringPtr, "lblog=")) { g_ldmBucketSizeLog = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        if (longCommandWArg(&stringPtr, "ldmHashRateLog=") || longCommandWArg(&stringPtr, "lhrlog=")) { g_ldmHashRateLog = readU32FromChar(&stringPtr); if (stringPtr[0]==',') { stringPtr++; continue; } else break; }
        DISPLAYLEVEL(4, "invalid compression parameter \n");
        return 0;
    }

    DISPLAYLEVEL(4, "windowLog=%d, chainLog=%d, hashLog=%d, searchLog=%d \n", params->windowLog, params->chainLog, params->hashLog, params->searchLog);
    DISPLAYLEVEL(4, "minMatch=%d, targetLength=%d, strategy=%d \n", params->minMatch, params->targetLength, params->strategy);
    if (stringPtr[0] != 0) return 0; /* check the end of string */
    return 1;
}

static void printVersion(void)
{
    if (g_displayLevel < DISPLAY_LEVEL_DEFAULT) {
        DISPLAYOUT("%s\n", ZSTD_VERSION_STRING);
        return;
    }

    DISPLAYOUT(WELCOME_MESSAGE);
    if (g_displayLevel >= 3) {
    /* format support */
        DISPLAYOUT("*** supports: zstd");
    #if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT>0) && (ZSTD_LEGACY_SUPPORT<8)
        DISPLAYOUT(", zstd legacy v0.%d+", ZSTD_LEGACY_SUPPORT);
    #endif
    #ifdef ZSTD_GZCOMPRESS
        DISPLAYOUT(", gzip");
    #endif
    #ifdef ZSTD_LZ4COMPRESS
        DISPLAYOUT(", lz4");
    #endif
    #ifdef ZSTD_LZMACOMPRESS
        DISPLAYOUT(", lzma, xz ");
    #endif
        DISPLAYOUT("\n");
        if (g_displayLevel >= 4) {
            /* library versions */
            DISPLAYOUT("zlib version %s\n", FIO_zlibVersion());
            DISPLAYOUT("lz4 version %s\n", FIO_lz4Version());
            DISPLAYOUT("lzma version %s\n", FIO_lzmaVersion());

            /* posix support */
        #ifdef _POSIX_C_SOURCE
            DISPLAYOUT("_POSIX_C_SOURCE defined: %ldL\n", (long) _POSIX_C_SOURCE);
        #endif
        #ifdef _POSIX_VERSION
            DISPLAYOUT("_POSIX_VERSION defined: %ldL \n", (long) _POSIX_VERSION);
        #endif
        #ifdef PLATFORM_POSIX_VERSION
            DISPLAYOUT("PLATFORM_POSIX_VERSION defined: %ldL\n", (long) PLATFORM_POSIX_VERSION);
        #endif
    }   }
}

#define ZSTD_NB_STRATEGIES 9
static const char* ZSTD_strategyMap[ZSTD_NB_STRATEGIES + 1] = { "", "ZSTD_fast",
                "ZSTD_dfast", "ZSTD_greedy", "ZSTD_lazy", "ZSTD_lazy2", "ZSTD_btlazy2",
                "ZSTD_btopt", "ZSTD_btultra", "ZSTD_btultra2"};

#ifndef ZSTD_NOCOMPRESS

static void printDefaultCParams(const char* filename, const char* dictFileName, int cLevel) {
    unsigned long long fileSize = UTIL_getFileSize(filename);
    const size_t dictSize = dictFileName != NULL ? (size_t)UTIL_getFileSize(dictFileName) : 0;
    const ZSTD_compressionParameters cParams = ZSTD_getCParams(cLevel, fileSize, dictSize);
    if (fileSize != UTIL_FILESIZE_UNKNOWN) DISPLAY("%s (%u bytes)\n", filename, (unsigned)fileSize);
    else DISPLAY("%s (src size unknown)\n", filename);
    DISPLAY(" - windowLog     : %u\n", cParams.windowLog);
    DISPLAY(" - chainLog      : %u\n", cParams.chainLog);
    DISPLAY(" - hashLog       : %u\n", cParams.hashLog);
    DISPLAY(" - searchLog     : %u\n", cParams.searchLog);
    DISPLAY(" - minMatch      : %u\n", cParams.minMatch);
    DISPLAY(" - targetLength  : %u\n", cParams.targetLength);
    assert(cParams.strategy < ZSTD_NB_STRATEGIES + 1);
    DISPLAY(" - strategy      : %s (%u)\n", ZSTD_strategyMap[(int)cParams.strategy], (unsigned)cParams.strategy);
}

static void printActualCParams(const char* filename, const char* dictFileName, int cLevel, const ZSTD_compressionParameters* cParams) {
    unsigned long long fileSize = UTIL_getFileSize(filename);
    const size_t dictSize = dictFileName != NULL ? (size_t)UTIL_getFileSize(dictFileName) : 0;
    ZSTD_compressionParameters actualCParams = ZSTD_getCParams(cLevel, fileSize, dictSize);
    assert(g_displayLevel >= 4);
    actualCParams.windowLog = cParams->windowLog == 0 ? actualCParams.windowLog : cParams->windowLog;
    actualCParams.chainLog = cParams->chainLog == 0 ? actualCParams.chainLog : cParams->chainLog;
    actualCParams.hashLog = cParams->hashLog == 0 ? actualCParams.hashLog : cParams->hashLog;
    actualCParams.searchLog = cParams->searchLog == 0 ? actualCParams.searchLog : cParams->searchLog;
    actualCParams.minMatch = cParams->minMatch == 0 ? actualCParams.minMatch : cParams->minMatch;
    actualCParams.targetLength = cParams->targetLength == 0 ? actualCParams.targetLength : cParams->targetLength;
    actualCParams.strategy = cParams->strategy == 0 ? actualCParams.strategy : cParams->strategy;
    DISPLAY("--zstd=wlog=%d,clog=%d,hlog=%d,slog=%d,mml=%d,tlen=%d,strat=%d\n",
            actualCParams.windowLog, actualCParams.chainLog, actualCParams.hashLog, actualCParams.searchLog,
            actualCParams.minMatch, actualCParams.targetLength, actualCParams.strategy);
}

#endif

/* Environment variables for parameter setting */
#define ENV_CLEVEL "ZSTD_CLEVEL"
#define ENV_NBTHREADS "ZSTD_NBTHREADS"    /* takes lower precedence than directly specifying -T# in the CLI */

/* pick up environment variable */
static int init_cLevel(void) {
    const char* const env = getenv(ENV_CLEVEL);
    if (env != NULL) {
        const char* ptr = env;
        int sign = 1;
        if (*ptr == '-') {
            sign = -1;
            ptr++;
        } else if (*ptr == '+') {
            ptr++;
        }

        if ((*ptr>='0') && (*ptr<='9')) {
            unsigned absLevel;
            if (readU32FromCharChecked(&ptr, &absLevel)) {
                DISPLAYLEVEL(2, "Ignore environment variable setting %s=%s: numeric value too large \n", ENV_CLEVEL, env);
                return ZSTDCLI_CLEVEL_DEFAULT;
            } else if (*ptr == 0) {
                return sign * (int)absLevel;
        }   }

        DISPLAYLEVEL(2, "Ignore environment variable setting %s=%s: not a valid integer value \n", ENV_CLEVEL, env);
    }

    return ZSTDCLI_CLEVEL_DEFAULT;
}

#ifdef ZSTD_MULTITHREAD
static unsigned init_nbThreads(void) {
    const char* const env = getenv(ENV_NBTHREADS);
    if (env != NULL) {
        const char* ptr = env;
        if ((*ptr>='0') && (*ptr<='9')) {
            unsigned nbThreads;
            if (readU32FromCharChecked(&ptr, &nbThreads)) {
                DISPLAYLEVEL(2, "Ignore environment variable setting %s=%s: numeric value too large \n", ENV_NBTHREADS, env);
                return ZSTDCLI_NBTHREADS_DEFAULT;
            } else if (*ptr == 0) {
                return nbThreads;
            }
        }
        DISPLAYLEVEL(2, "Ignore environment variable setting %s=%s: not a valid unsigned value \n", ENV_NBTHREADS, env);
    }

    return ZSTDCLI_NBTHREADS_DEFAULT;
}
#endif

#define NEXT_FIELD(ptr) {         \
    if (*argument == '=') {       \
        ptr = ++argument;         \
        argument += strlen(ptr);  \
    } else {                      \
        argNb++;                  \
        if (argNb >= argCount) {  \
            DISPLAYLEVEL(1, "error: missing command argument \n"); \
            CLEAN_RETURN(1);      \
        }                         \
        ptr = argv[argNb];        \
        assert(ptr != NULL);      \
        if (ptr[0]=='-') {        \
            DISPLAYLEVEL(1, "error: command cannot be separated from its argument by another command \n"); \
            CLEAN_RETURN(1);      \
}   }   }

#define NEXT_UINT32(val32) {      \
    const char* __nb;             \
    NEXT_FIELD(__nb);             \
    val32 = readU32FromChar(&__nb); \
    if(*__nb != 0) {         \
        errorOut("error: only numeric values with optional suffixes K, KB, KiB, M, MB, MiB are allowed"); \
    }                             \
}

#define NEXT_TSIZE(valTsize) {      \
    const char* __nb;             \
    NEXT_FIELD(__nb);             \
    valTsize = readSizeTFromChar(&__nb); \
    if(*__nb != 0) {         \
        errorOut("error: only numeric values with optional suffixes K, KB, KiB, M, MB, MiB are allowed"); \
    }                             \
}

typedef enum { zom_compress, zom_decompress, zom_test, zom_bench, zom_train, zom_list } zstd_operation_mode;

#define CLEAN_RETURN(i) { operationResult = (i); goto _end; }

#ifdef ZSTD_NOCOMPRESS
/* symbols from compression library are not defined and should not be invoked */
# define MINCLEVEL  -99
# define MAXCLEVEL   22
#else
# define MINCLEVEL  ZSTD_minCLevel()
# define MAXCLEVEL  ZSTD_maxCLevel()
#endif

int main(int argCount, const char* argv[])
{
    int argNb,
        followLinks = 0,
        allowBlockDevices = 0,
        forceStdin = 0,
        forceStdout = 0,
        hasStdout = 0,
        ldmFlag = 0,
        main_pause = 0,
        adapt = 0,
        adaptMin = MINCLEVEL,
        adaptMax = MAXCLEVEL,
        rsyncable = 0,
        nextArgumentsAreFiles = 0,
        operationResult = 0,
        separateFiles = 0,
        setRealTimePrio = 0,
        singleThread = 0,
        defaultLogicalCores = 0,
        showDefaultCParams = 0,
        ultra=0,
        contentSize=1,
        removeSrcFile=0;
    ZSTD_paramSwitch_e mmapDict=ZSTD_ps_auto;
    ZSTD_paramSwitch_e useRowMatchFinder = ZSTD_ps_auto;
    FIO_compressionType_t cType = FIO_zstdCompression;
    unsigned nbWorkers = 0;
    double compressibility = 0.5;
    unsigned bench_nbSeconds = 3;   /* would be better if this value was synchronized from bench */
    size_t blockSize = 0;

    FIO_prefs_t* const prefs = FIO_createPreferences();
    FIO_ctx_t* const fCtx = FIO_createContext();
    FIO_progressSetting_e progress = FIO_ps_auto;
    zstd_operation_mode operation = zom_compress;
    ZSTD_compressionParameters compressionParams;
    int cLevel = init_cLevel();
    int cLevelLast = MINCLEVEL - 1;  /* lower than minimum */
    unsigned recursive = 0;
    unsigned memLimit = 0;
    FileNamesTable* filenames = UTIL_allocateFileNamesTable((size_t)argCount);  /* argCount >= 1 */
    FileNamesTable* file_of_names = UTIL_allocateFileNamesTable((size_t)argCount);  /* argCount >= 1 */
    const char* programName = argv[0];
    const char* outFileName = NULL;
    const char* outDirName = NULL;
    const char* outMirroredDirName = NULL;
    const char* dictFileName = NULL;
    const char* patchFromDictFileName = NULL;
    const char* suffix = ZSTD_EXTENSION;
    unsigned maxDictSize = g_defaultMaxDictSize;
    unsigned dictID = 0;
    size_t streamSrcSize = 0;
    size_t targetCBlockSize = 0;
    size_t srcSizeHint = 0;
    size_t nbInputFileNames = 0;
    int dictCLevel = g_defaultDictCLevel;
    unsigned dictSelect = g_defaultSelectivityLevel;
#ifndef ZSTD_NODICT
    ZDICT_cover_params_t coverParams = defaultCoverParams();
    ZDICT_fastCover_params_t fastCoverParams = defaultFastCoverParams();
    dictType dict = fastCover;
#endif
#ifndef ZSTD_NOBENCH
    BMK_advancedParams_t benchParams = BMK_initAdvancedParams();
#endif
    ZSTD_paramSwitch_e literalCompressionMode = ZSTD_ps_auto;


    /* init */
    checkLibVersion();
    (void)recursive; (void)cLevelLast;    /* not used when ZSTD_NOBENCH set */
    (void)memLimit;
    assert(argCount >= 1);
    if ((filenames==NULL) || (file_of_names==NULL)) { DISPLAYLEVEL(1, "zstd: allocation error \n"); exit(1); }
    programName = lastNameFromPath(programName);
#ifdef ZSTD_MULTITHREAD
    nbWorkers = init_nbThreads();
#endif

    /* preset behaviors */
    if (exeNameMatch(programName, ZSTD_ZSTDMT)) nbWorkers=0, singleThread=0;
    if (exeNameMatch(programName, ZSTD_UNZSTD)) operation=zom_decompress;
    if (exeNameMatch(programName, ZSTD_CAT)) { operation=zom_decompress; FIO_overwriteMode(prefs); forceStdout=1; followLinks=1; FIO_setPassThroughFlag(prefs, 1); outFileName=stdoutmark; g_displayLevel=1; }     /* supports multiple formats */
    if (exeNameMatch(programName, ZSTD_ZCAT)) { operation=zom_decompress; FIO_overwriteMode(prefs); forceStdout=1; followLinks=1; FIO_setPassThroughFlag(prefs, 1); outFileName=stdoutmark; g_displayLevel=1; }    /* behave like zcat, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_GZ)) {   /* behave like gzip */
        suffix = GZ_EXTENSION; cType = FIO_gzipCompression; removeSrcFile=1;
        dictCLevel = cLevel = 6;  /* gzip default is -6 */
    }
    if (exeNameMatch(programName, ZSTD_GUNZIP)) { operation=zom_decompress; removeSrcFile=1; }                                                     /* behave like gunzip, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_GZCAT)) { operation=zom_decompress; FIO_overwriteMode(prefs); forceStdout=1; followLinks=1; FIO_setPassThroughFlag(prefs, 1); outFileName=stdoutmark; g_displayLevel=1; }   /* behave like gzcat, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_LZMA)) { suffix = LZMA_EXTENSION; cType = FIO_lzmaCompression; removeSrcFile=1; }    /* behave like lzma */
    if (exeNameMatch(programName, ZSTD_UNLZMA)) { operation=zom_decompress; cType = FIO_lzmaCompression; removeSrcFile=1; } /* behave like unlzma, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_XZ)) { suffix = XZ_EXTENSION; cType = FIO_xzCompression; removeSrcFile=1; }          /* behave like xz */
    if (exeNameMatch(programName, ZSTD_UNXZ)) { operation=zom_decompress; cType = FIO_xzCompression; removeSrcFile=1; }     /* behave like unxz, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_LZ4)) { suffix = LZ4_EXTENSION; cType = FIO_lz4Compression; }                        /* behave like lz4 */
    if (exeNameMatch(programName, ZSTD_UNLZ4)) { operation=zom_decompress; cType = FIO_lz4Compression; }                    /* behave like unlz4, also supports multiple formats */
    memset(&compressionParams, 0, sizeof(compressionParams));

    /* init crash handler */
    FIO_addAbortHandler();

    /* command switches */
    for (argNb=1; argNb<argCount; argNb++) {
        const char* argument = argv[argNb];
        if (!argument) continue;   /* Protection if argument empty */

        if (nextArgumentsAreFiles) {
            UTIL_refFilename(filenames, argument);
            continue;
        }

        /* "-" means stdin/stdout */
        if (!strcmp(argument, "-")){
            UTIL_refFilename(filenames, stdinmark);
            continue;
        }

        /* Decode commands (note : aggregated commands are allowed) */
        if (argument[0]=='-') {

            if (argument[1]=='-') {
                /* long commands (--long-word) */
                if (!strcmp(argument, "--")) { nextArgumentsAreFiles=1; continue; }   /* only file names allowed from now on */
                if (!strcmp(argument, "--list")) { operation=zom_list; continue; }
                if (!strcmp(argument, "--compress")) { operation=zom_compress; continue; }
                if (!strcmp(argument, "--decompress")) { operation=zom_decompress; continue; }
                if (!strcmp(argument, "--uncompress")) { operation=zom_decompress; continue; }
                if (!strcmp(argument, "--force")) { FIO_overwriteMode(prefs); forceStdin=1; forceStdout=1; followLinks=1; allowBlockDevices=1; continue; }
                if (!strcmp(argument, "--version")) { printVersion(); CLEAN_RETURN(0); }
                if (!strcmp(argument, "--help")) { usage_advanced(programName); CLEAN_RETURN(0); }
                if (!strcmp(argument, "--verbose")) { g_displayLevel++; continue; }
                if (!strcmp(argument, "--quiet")) { g_displayLevel--; continue; }
                if (!strcmp(argument, "--stdout")) { forceStdout=1; outFileName=stdoutmark; removeSrcFile=0; continue; }
                if (!strcmp(argument, "--ultra")) { ultra=1; continue; }
                if (!strcmp(argument, "--check")) { FIO_setChecksumFlag(prefs, 2); continue; }
                if (!strcmp(argument, "--no-check")) { FIO_setChecksumFlag(prefs, 0); continue; }
                if (!strcmp(argument, "--sparse")) { FIO_setSparseWrite(prefs, 2); continue; }
                if (!strcmp(argument, "--no-sparse")) { FIO_setSparseWrite(prefs, 0); continue; }
                if (!strcmp(argument, "--pass-through")) { FIO_setPassThroughFlag(prefs, 1); continue; }
                if (!strcmp(argument, "--no-pass-through")) { FIO_setPassThroughFlag(prefs, 0); continue; }
                if (!strcmp(argument, "--test")) { operation=zom_test; continue; }
                if (!strcmp(argument, "--asyncio")) { FIO_setAsyncIOFlag(prefs, 1); continue;}
                if (!strcmp(argument, "--no-asyncio")) { FIO_setAsyncIOFlag(prefs, 0); continue;}
                if (!strcmp(argument, "--train")) { operation=zom_train; if (outFileName==NULL) outFileName=g_defaultDictName; continue; }
                if (!strcmp(argument, "--no-dictID")) { FIO_setDictIDFlag(prefs, 0); continue; }
                if (!strcmp(argument, "--keep")) { removeSrcFile=0; continue; }
                if (!strcmp(argument, "--rm")) { removeSrcFile=1; continue; }
                if (!strcmp(argument, "--priority=rt")) { setRealTimePrio = 1; continue; }
                if (!strcmp(argument, "--show-default-cparams")) { showDefaultCParams = 1; continue; }
                if (!strcmp(argument, "--content-size")) { contentSize = 1; continue; }
                if (!strcmp(argument, "--no-content-size")) { contentSize = 0; continue; }
                if (!strcmp(argument, "--adapt")) { adapt = 1; continue; }
                if (!strcmp(argument, "--no-row-match-finder")) { useRowMatchFinder = ZSTD_ps_disable; continue; }
                if (!strcmp(argument, "--row-match-finder")) { useRowMatchFinder = ZSTD_ps_enable; continue; }
                if (longCommandWArg(&argument, "--adapt=")) { adapt = 1; if (!parseAdaptParameters(argument, &adaptMin, &adaptMax)) { badusage(programName); CLEAN_RETURN(1); } continue; }
                if (!strcmp(argument, "--single-thread")) { nbWorkers = 0; singleThread = 1; continue; }
                if (!strcmp(argument, "--format=zstd")) { suffix = ZSTD_EXTENSION; cType = FIO_zstdCompression; continue; }
                if (!strcmp(argument, "--mmap-dict")) { mmapDict = ZSTD_ps_enable; continue; }
                if (!strcmp(argument, "--no-mmap-dict")) { mmapDict = ZSTD_ps_disable; continue; }
#ifdef ZSTD_GZCOMPRESS
                if (!strcmp(argument, "--format=gzip")) { suffix = GZ_EXTENSION; cType = FIO_gzipCompression; continue; }
                if (exeNameMatch(programName, ZSTD_GZ)) {     /* behave like gzip */
                    if (!strcmp(argument, "--best")) { dictCLevel = cLevel = 9; continue; }
                    if (!strcmp(argument, "--no-name")) { /* ignore for now */; continue; }
                }
#endif
#ifdef ZSTD_LZMACOMPRESS
                if (!strcmp(argument, "--format=lzma")) { suffix = LZMA_EXTENSION; cType = FIO_lzmaCompression; continue; }
                if (!strcmp(argument, "--format=xz")) { suffix = XZ_EXTENSION; cType = FIO_xzCompression; continue; }
#endif
#ifdef ZSTD_LZ4COMPRESS
                if (!strcmp(argument, "--format=lz4")) { suffix = LZ4_EXTENSION; cType = FIO_lz4Compression; continue; }
#endif
                if (!strcmp(argument, "--rsyncable")) { rsyncable = 1; continue; }
                if (!strcmp(argument, "--compress-literals")) { literalCompressionMode = ZSTD_ps_enable; continue; }
                if (!strcmp(argument, "--no-compress-literals")) { literalCompressionMode = ZSTD_ps_disable; continue; }
                if (!strcmp(argument, "--no-progress")) { progress = FIO_ps_never; continue; }
                if (!strcmp(argument, "--progress")) { progress = FIO_ps_always; continue; }
                if (!strcmp(argument, "--exclude-compressed")) { FIO_setExcludeCompressedFile(prefs, 1); continue; }
                if (!strcmp(argument, "--fake-stdin-is-console")) { UTIL_fakeStdinIsConsole(); continue; }
                if (!strcmp(argument, "--fake-stdout-is-console")) { UTIL_fakeStdoutIsConsole(); continue; }
                if (!strcmp(argument, "--fake-stderr-is-console")) { UTIL_fakeStderrIsConsole(); continue; }
                if (!strcmp(argument, "--trace-file-stat")) { UTIL_traceFileStat(); continue; }

                /* long commands with arguments */
#ifndef ZSTD_NODICT
                if (longCommandWArg(&argument, "--train-cover")) {
                  operation = zom_train;
                  if (outFileName == NULL)
                      outFileName = g_defaultDictName;
                  dict = cover;
                  /* Allow optional arguments following an = */
                  if (*argument == 0) { memset(&coverParams, 0, sizeof(coverParams)); }
                  else if (*argument++ != '=') { badusage(programName); CLEAN_RETURN(1); }
                  else if (!parseCoverParameters(argument, &coverParams)) { badusage(programName); CLEAN_RETURN(1); }
                  continue;
                }
                if (longCommandWArg(&argument, "--train-fastcover")) {
                  operation = zom_train;
                  if (outFileName == NULL)
                      outFileName = g_defaultDictName;
                  dict = fastCover;
                  /* Allow optional arguments following an = */
                  if (*argument == 0) { memset(&fastCoverParams, 0, sizeof(fastCoverParams)); }
                  else if (*argument++ != '=') { badusage(programName); CLEAN_RETURN(1); }
                  else if (!parseFastCoverParameters(argument, &fastCoverParams)) { badusage(programName); CLEAN_RETURN(1); }
                  continue;
                }
                if (longCommandWArg(&argument, "--train-legacy")) {
                  operation = zom_train;
                  if (outFileName == NULL)
                      outFileName = g_defaultDictName;
                  dict = legacy;
                  /* Allow optional arguments following an = */
                  if (*argument == 0) { continue; }
                  else if (*argument++ != '=') { badusage(programName); CLEAN_RETURN(1); }
                  else if (!parseLegacyParameters(argument, &dictSelect)) { badusage(programName); CLEAN_RETURN(1); }
                  continue;
                }
#endif
                if (longCommandWArg(&argument, "--threads")) { NEXT_UINT32(nbWorkers); continue; }
                if (longCommandWArg(&argument, "--memlimit")) { NEXT_UINT32(memLimit); continue; }
                if (longCommandWArg(&argument, "--memory")) { NEXT_UINT32(memLimit); continue; }
                if (longCommandWArg(&argument, "--memlimit-decompress")) { NEXT_UINT32(memLimit); continue; }
                if (longCommandWArg(&argument, "--block-size")) { NEXT_TSIZE(blockSize); continue; }
                if (longCommandWArg(&argument, "--maxdict")) { NEXT_UINT32(maxDictSize); continue; }
                if (longCommandWArg(&argument, "--dictID")) { NEXT_UINT32(dictID); continue; }
                if (longCommandWArg(&argument, "--zstd=")) { if (!parseCompressionParameters(argument, &compressionParams)) { badusage(programName); CLEAN_RETURN(1); } ; cType = FIO_zstdCompression; continue; }
                if (longCommandWArg(&argument, "--stream-size")) { NEXT_TSIZE(streamSrcSize); continue; }
                if (longCommandWArg(&argument, "--target-compressed-block-size")) { NEXT_TSIZE(targetCBlockSize); continue; }
                if (longCommandWArg(&argument, "--size-hint")) { NEXT_TSIZE(srcSizeHint); continue; }
                if (longCommandWArg(&argument, "--output-dir-flat")) {
                    NEXT_FIELD(outDirName);
                    if (strlen(outDirName) == 0) {
                        DISPLAYLEVEL(1, "error: output dir cannot be empty string (did you mean to pass '.' instead?)\n");
                        CLEAN_RETURN(1);
                    }
                    continue;
                }
                if (longCommandWArg(&argument, "--auto-threads")) {
                    const char* threadDefault = NULL;
                    NEXT_FIELD(threadDefault);
                    if (strcmp(threadDefault, "logical") == 0)
                        defaultLogicalCores = 1;
                    continue;
                }
#ifdef UTIL_HAS_MIRRORFILELIST
                if (longCommandWArg(&argument, "--output-dir-mirror")) {
                    NEXT_FIELD(outMirroredDirName);
                    if (strlen(outMirroredDirName) == 0) {
                        DISPLAYLEVEL(1, "error: output dir cannot be empty string (did you mean to pass '.' instead?)\n");
                        CLEAN_RETURN(1);
                    }
                    continue;
                }
#endif
#ifndef ZSTD_NOTRACE
                if (longCommandWArg(&argument, "--trace")) { char const* traceFile; NEXT_FIELD(traceFile); TRACE_enable(traceFile); continue; }
#endif
                if (longCommandWArg(&argument, "--patch-from")) { NEXT_FIELD(patchFromDictFileName); continue; }
                if (longCommandWArg(&argument, "--long")) {
                    unsigned ldmWindowLog = 0;
                    ldmFlag = 1;
                    /* Parse optional window log */
                    if (*argument == '=') {
                        ++argument;
                        ldmWindowLog = readU32FromChar(&argument);
                    } else if (*argument != 0) {
                        /* Invalid character following --long */
                        badusage(programName);
                        CLEAN_RETURN(1);
                    } else {
                        ldmWindowLog = g_defaultMaxWindowLog;
                    }
                    /* Only set windowLog if not already set by --zstd */
                    if (compressionParams.windowLog == 0)
                        compressionParams.windowLog = ldmWindowLog;
                    continue;
                }
#ifndef ZSTD_NOCOMPRESS   /* linking ZSTD_minCLevel() requires compression support */
                if (longCommandWArg(&argument, "--fast")) {
                    /* Parse optional acceleration factor */
                    if (*argument == '=') {
                        U32 const maxFast = (U32)-ZSTD_minCLevel();
                        U32 fastLevel;
                        ++argument;
                        fastLevel = readU32FromChar(&argument);
                        if (fastLevel > maxFast) fastLevel = maxFast;
                        if (fastLevel) {
                            dictCLevel = cLevel = -(int)fastLevel;
                        } else {
                            badusage(programName);
                            CLEAN_RETURN(1);
                        }
                    } else if (*argument != 0) {
                        /* Invalid character following --fast */
                        badusage(programName);
                        CLEAN_RETURN(1);
                    } else {
                        cLevel = -1;  /* default for --fast */
                    }
                    continue;
                }
#endif

                if (longCommandWArg(&argument, "--filelist")) {
                    const char* listName;
                    NEXT_FIELD(listName);
                    UTIL_refFilename(file_of_names, listName);
                    continue;
                }

                /* fall-through, will trigger bad_usage() later on */
            }

            argument++;
            while (argument[0]!=0) {

#ifndef ZSTD_NOCOMPRESS
                /* compression Level */
                if ((*argument>='0') && (*argument<='9')) {
                    dictCLevel = cLevel = (int)readU32FromChar(&argument);
                    continue;
                }
#endif

                switch(argument[0])
                {
                    /* Display help */
                case 'V': printVersion(); CLEAN_RETURN(0);   /* Version Only */
                case 'H': usage_advanced(programName); CLEAN_RETURN(0);
                case 'h': usage(stdout, programName); CLEAN_RETURN(0);

                     /* Compress */
                case 'z': operation=zom_compress; argument++; break;

                     /* Decoding */
                case 'd':
#ifndef ZSTD_NOBENCH
                        benchParams.mode = BMK_decodeOnly;
                        if (operation==zom_bench) { argument++; break; }  /* benchmark decode (hidden option) */
#endif
                        operation=zom_decompress; argument++; break;

                    /* Force stdout, even if stdout==console */
                case 'c': forceStdout=1; outFileName=stdoutmark; removeSrcFile=0; argument++; break;

                    /* do not store filename - gzip compatibility - nothing to do */
                case 'n': argument++; break;

                    /* Use file content as dictionary */
                case 'D': argument++; NEXT_FIELD(dictFileName); break;

                    /* Overwrite */
                case 'f': FIO_overwriteMode(prefs); forceStdin=1; forceStdout=1; followLinks=1; allowBlockDevices=1; argument++; break;

                    /* Verbose mode */
                case 'v': g_displayLevel++; argument++; break;

                    /* Quiet mode */
                case 'q': g_displayLevel--; argument++; break;

                    /* keep source file (default) */
                case 'k': removeSrcFile=0; argument++; break;

                    /* Checksum */
                case 'C': FIO_setChecksumFlag(prefs, 2); argument++; break;

                    /* test compressed file */
                case 't': operation=zom_test; argument++; break;

                    /* destination file name */
                case 'o': argument++; NEXT_FIELD(outFileName); break;

                    /* limit memory */
                case 'M':
                    argument++;
                    memLimit = readU32FromChar(&argument);
                    break;
                case 'l': operation=zom_list; argument++; break;
#ifdef UTIL_HAS_CREATEFILELIST
                    /* recursive */
                case 'r': recursive=1; argument++; break;
#endif

#ifndef ZSTD_NOBENCH
                    /* Benchmark */
                case 'b':
                    operation=zom_bench;
                    argument++;
                    break;

                    /* range bench (benchmark only) */
                case 'e':
                    /* compression Level */
                    argument++;
                    cLevelLast = (int)readU32FromChar(&argument);
                    break;

                    /* Modify Nb Iterations (benchmark only) */
                case 'i':
                    argument++;
                    bench_nbSeconds = readU32FromChar(&argument);
                    break;

                    /* cut input into blocks (benchmark only) */
                case 'B':
                    argument++;
                    blockSize = readU32FromChar(&argument);
                    break;

                    /* benchmark files separately (hidden option) */
                case 'S':
                    argument++;
                    separateFiles = 1;
                    break;

#endif   /* ZSTD_NOBENCH */

                    /* nb of threads (hidden option) */
                case 'T':
                    argument++;
                    nbWorkers = readU32FromChar(&argument);
                    break;

                    /* Dictionary Selection level */
                case 's':
                    argument++;
                    dictSelect = readU32FromChar(&argument);
                    break;

                    /* Pause at the end (-p) or set an additional param (-p#) (hidden option) */
                case 'p': argument++;
#ifndef ZSTD_NOBENCH
                    if ((*argument>='0') && (*argument<='9')) {
                        benchParams.additionalParam = (int)readU32FromChar(&argument);
                    } else
#endif
                        main_pause=1;
                    break;

                    /* Select compressibility of synthetic sample */
                case 'P':
                    argument++;
                    compressibility = (double)readU32FromChar(&argument) / 100;
                    break;

                    /* unknown command */
                default : badusage(programName); CLEAN_RETURN(1);
                }
            }
            continue;
        }   /* if (argument[0]=='-') */

        /* none of the above : add filename to list */
        UTIL_refFilename(filenames, argument);
    }

    /* Welcome message (if verbose) */
    DISPLAYLEVEL(3, WELCOME_MESSAGE);

#ifdef ZSTD_MULTITHREAD
    if ((operation==zom_decompress) && (!singleThread) && (nbWorkers > 1)) {
        DISPLAYLEVEL(2, "Warning : decompression does not support multi-threading\n");
    }
    if ((nbWorkers==0) && (!singleThread)) {
        /* automatically set # workers based on # of reported cpus */
        if (defaultLogicalCores) {
            nbWorkers = (unsigned)UTIL_countLogicalCores();
            DISPLAYLEVEL(3, "Note: %d logical core(s) detected \n", nbWorkers);
        } else {
            nbWorkers = (unsigned)UTIL_countPhysicalCores();
            DISPLAYLEVEL(3, "Note: %d physical core(s) detected \n", nbWorkers);
        }
    }
#else
    (void)singleThread; (void)nbWorkers; (void)defaultLogicalCores;
#endif

    g_utilDisplayLevel = g_displayLevel;

#ifdef UTIL_HAS_CREATEFILELIST
    if (!followLinks) {
        unsigned u, fileNamesNb;
        unsigned const nbFilenames = (unsigned)filenames->tableSize;
        for (u=0, fileNamesNb=0; u<nbFilenames; u++) {
            if ( UTIL_isLink(filenames->fileNames[u])
             && !UTIL_isFIFO(filenames->fileNames[u])
            ) {
                DISPLAYLEVEL(2, "Warning : %s is a symbolic link, ignoring \n", filenames->fileNames[u]);
            } else {
                filenames->fileNames[fileNamesNb++] = filenames->fileNames[u];
        }   }
        if (fileNamesNb == 0 && nbFilenames > 0)  /* all names are eliminated */
            CLEAN_RETURN(1);
        filenames->tableSize = fileNamesNb;
    }   /* if (!followLinks) */

    /* read names from a file */
    if (file_of_names->tableSize) {
        size_t const nbFileLists = file_of_names->tableSize;
        size_t flNb;
        for (flNb=0; flNb < nbFileLists; flNb++) {
            FileNamesTable* const fnt = UTIL_createFileNamesTable_fromFileName(file_of_names->fileNames[flNb]);
            if (fnt==NULL) {
                DISPLAYLEVEL(1, "zstd: error reading %s \n", file_of_names->fileNames[flNb]);
                CLEAN_RETURN(1);
            }
            filenames = UTIL_mergeFileNamesTable(filenames, fnt);
        }
    }

    nbInputFileNames = filenames->tableSize; /* saving number of input files */

    if (recursive) {  /* at this stage, filenameTable is a list of paths, which can contain both files and directories */
        UTIL_expandFNT(&filenames, followLinks);
    }
#else
    (void)followLinks;
#endif

    if (operation == zom_list) {
#ifndef ZSTD_NODECOMPRESS
        int const ret = FIO_listMultipleFiles((unsigned)filenames->tableSize, filenames->fileNames, g_displayLevel);
        CLEAN_RETURN(ret);
#else
        DISPLAYLEVEL(1, "file information is not supported \n");
        CLEAN_RETURN(1);
#endif
    }

    /* Check if benchmark is selected */
    if (operation==zom_bench) {
#ifndef ZSTD_NOBENCH
        if (cType != FIO_zstdCompression) {
            DISPLAYLEVEL(1, "benchmark mode is only compatible with zstd format \n");
            CLEAN_RETURN(1);
        }
        benchParams.blockSize = blockSize;
        benchParams.nbWorkers = (int)nbWorkers;
        benchParams.realTime = (unsigned)setRealTimePrio;
        benchParams.nbSeconds = bench_nbSeconds;
        benchParams.ldmFlag = ldmFlag;
        benchParams.ldmMinMatch = (int)g_ldmMinMatch;
        benchParams.ldmHashLog = (int)g_ldmHashLog;
        benchParams.useRowMatchFinder = (int)useRowMatchFinder;
        if (g_ldmBucketSizeLog != LDM_PARAM_DEFAULT) {
            benchParams.ldmBucketSizeLog = (int)g_ldmBucketSizeLog;
        }
        if (g_ldmHashRateLog != LDM_PARAM_DEFAULT) {
            benchParams.ldmHashRateLog = (int)g_ldmHashRateLog;
        }
        benchParams.literalCompressionMode = literalCompressionMode;

        if (cLevel > ZSTD_maxCLevel()) cLevel = ZSTD_maxCLevel();
        if (cLevelLast > ZSTD_maxCLevel()) cLevelLast = ZSTD_maxCLevel();
        if (cLevelLast < cLevel) cLevelLast = cLevel;
        if (cLevelLast > cLevel)
            DISPLAYLEVEL(3, "Benchmarking levels from %d to %d\n", cLevel, cLevelLast);
        if (filenames->tableSize > 0) {
            if(separateFiles) {
                unsigned i;
                for(i = 0; i < filenames->tableSize; i++) {
                    int c;
                    DISPLAYLEVEL(3, "Benchmarking %s \n", filenames->fileNames[i]);
                    for(c = cLevel; c <= cLevelLast; c++) {
                        operationResult = BMK_benchFilesAdvanced(&filenames->fileNames[i], 1, dictFileName, c, &compressionParams, g_displayLevel, &benchParams);
                }   }
            } else {
                for(; cLevel <= cLevelLast; cLevel++) {
                    operationResult = BMK_benchFilesAdvanced(filenames->fileNames, (unsigned)filenames->tableSize, dictFileName, cLevel, &compressionParams, g_displayLevel, &benchParams);
            }   }
        } else {
            for(; cLevel <= cLevelLast; cLevel++) {
                operationResult = BMK_syntheticTest(cLevel, compressibility, &compressionParams, g_displayLevel, &benchParams);
        }   }

#else
        (void)bench_nbSeconds; (void)blockSize; (void)setRealTimePrio; (void)separateFiles; (void)compressibility;
#endif
        goto _end;
    }

    /* Check if dictionary builder is selected */
    if (operation==zom_train) {
#ifndef ZSTD_NODICT
        ZDICT_params_t zParams;
        zParams.compressionLevel = dictCLevel;
        zParams.notificationLevel = (unsigned)g_displayLevel;
        zParams.dictID = dictID;
        if (dict == cover) {
            int const optimize = !coverParams.k || !coverParams.d;
            coverParams.nbThreads = (unsigned)nbWorkers;
            coverParams.zParams = zParams;
            operationResult = DiB_trainFromFiles(outFileName, maxDictSize, filenames->fileNames, (int)filenames->tableSize, blockSize, NULL, &coverParams, NULL, optimize, memLimit);
        } else if (dict == fastCover) {
            int const optimize = !fastCoverParams.k || !fastCoverParams.d;
            fastCoverParams.nbThreads = (unsigned)nbWorkers;
            fastCoverParams.zParams = zParams;
            operationResult = DiB_trainFromFiles(outFileName, maxDictSize, filenames->fileNames, (int)filenames->tableSize, blockSize, NULL, NULL, &fastCoverParams, optimize, memLimit);
        } else {
            ZDICT_legacy_params_t dictParams;
            memset(&dictParams, 0, sizeof(dictParams));
            dictParams.selectivityLevel = dictSelect;
            dictParams.zParams = zParams;
            operationResult = DiB_trainFromFiles(outFileName, maxDictSize, filenames->fileNames, (int)filenames->tableSize, blockSize, &dictParams, NULL, NULL, 0, memLimit);
        }
#else
        (void)dictCLevel; (void)dictSelect; (void)dictID;  (void)maxDictSize; /* not used when ZSTD_NODICT set */
        DISPLAYLEVEL(1, "training mode not available \n");
        operationResult = 1;
#endif
        goto _end;
    }

#ifndef ZSTD_NODECOMPRESS
    if (operation==zom_test) { FIO_setTestMode(prefs, 1); outFileName=nulmark; removeSrcFile=0; }  /* test mode */
#endif

    /* No input filename ==> use stdin and stdout */
    if (filenames->tableSize == 0) {
      /* It is possible that the input
       was a number of empty directories. In this case
       stdin and stdout should not be used */
       if (nbInputFileNames > 0 ){
        DISPLAYLEVEL(1, "please provide correct input file(s) or non-empty directories -- ignored \n");
        CLEAN_RETURN(0);
       }
       UTIL_refFilename(filenames, stdinmark);
    }

    if (filenames->tableSize == 1 && !strcmp(filenames->fileNames[0], stdinmark) && !outFileName)
        outFileName = stdoutmark;  /* when input is stdin, default output is stdout */

    /* Check if input/output defined as console; trigger an error in this case */
    if (!forceStdin
     && (UTIL_searchFileNamesTable(filenames, stdinmark) != -1)
     && UTIL_isConsole(stdin) ) {
        DISPLAYLEVEL(1, "stdin is a console, aborting\n");
        CLEAN_RETURN(1);
    }
    if ( (!outFileName || !strcmp(outFileName, stdoutmark))
      && UTIL_isConsole(stdout)
      && (UTIL_searchFileNamesTable(filenames, stdinmark) != -1)
      && !forceStdout
      && operation!=zom_decompress ) {
        DISPLAYLEVEL(1, "stdout is a console, aborting\n");
        CLEAN_RETURN(1);
    }

#ifndef ZSTD_NOCOMPRESS
    /* check compression level limits */
    {   int const maxCLevel = ultra ? ZSTD_maxCLevel() : ZSTDCLI_CLEVEL_MAX;
        if (cLevel > maxCLevel) {
            DISPLAYLEVEL(2, "Warning : compression level higher than max, reduced to %i \n", maxCLevel);
            cLevel = maxCLevel;
    }   }
#endif

    if (showDefaultCParams) {
        if (operation == zom_decompress) {
            DISPLAYLEVEL(1, "error : can't use --show-default-cparams in decompression mode \n");
            CLEAN_RETURN(1);
        }
    }

    if (dictFileName != NULL && patchFromDictFileName != NULL) {
        DISPLAYLEVEL(1, "error : can't use -D and --patch-from=# at the same time \n");
        CLEAN_RETURN(1);
    }

    if (patchFromDictFileName != NULL && filenames->tableSize > 1) {
        DISPLAYLEVEL(1, "error : can't use --patch-from=# on multiple files \n");
        CLEAN_RETURN(1);
    }

    /* No status message by default when output is stdout */
    hasStdout = outFileName && !strcmp(outFileName,stdoutmark);
    if (hasStdout && (g_displayLevel==2)) g_displayLevel=1;

    /* when stderr is not the console, do not pollute it with progress updates (unless requested) */
    if (!UTIL_isConsole(stderr) && (progress!=FIO_ps_always)) progress=FIO_ps_never;
    FIO_setProgressSetting(progress);

    /* don't remove source files when output is stdout */;
    if (hasStdout && removeSrcFile) {
        DISPLAYLEVEL(3, "Note: src files are not removed when output is stdout \n");
        removeSrcFile = 0;
    }
    FIO_setRemoveSrcFile(prefs, removeSrcFile);

    /* IO Stream/File */
    FIO_setHasStdoutOutput(fCtx, hasStdout);
    FIO_setNbFilesTotal(fCtx, (int)filenames->tableSize);
    FIO_determineHasStdinInput(fCtx, filenames);
    FIO_setNotificationLevel(g_displayLevel);
    FIO_setAllowBlockDevices(prefs, allowBlockDevices);
    FIO_setPatchFromMode(prefs, patchFromDictFileName != NULL);
    FIO_setMMapDict(prefs, mmapDict);
    if (memLimit == 0) {
        if (compressionParams.windowLog == 0) {
            memLimit = (U32)1 << g_defaultMaxWindowLog;
        } else {
            memLimit = (U32)1 << (compressionParams.windowLog & 31);
    }   }
    if (patchFromDictFileName != NULL)
        dictFileName = patchFromDictFileName;
    FIO_setMemLimit(prefs, memLimit);
    if (operation==zom_compress) {
#ifndef ZSTD_NOCOMPRESS
        FIO_setCompressionType(prefs, cType);
        FIO_setContentSize(prefs, contentSize);
        FIO_setNbWorkers(prefs, (int)nbWorkers);
        FIO_setBlockSize(prefs, (int)blockSize);
        if (g_overlapLog!=OVERLAP_LOG_DEFAULT) FIO_setOverlapLog(prefs, (int)g_overlapLog);
        FIO_setLdmFlag(prefs, (unsigned)ldmFlag);
        FIO_setLdmHashLog(prefs, (int)g_ldmHashLog);
        FIO_setLdmMinMatch(prefs, (int)g_ldmMinMatch);
        if (g_ldmBucketSizeLog != LDM_PARAM_DEFAULT) FIO_setLdmBucketSizeLog(prefs, (int)g_ldmBucketSizeLog);
        if (g_ldmHashRateLog != LDM_PARAM_DEFAULT) FIO_setLdmHashRateLog(prefs, (int)g_ldmHashRateLog);
        FIO_setAdaptiveMode(prefs, adapt);
        FIO_setUseRowMatchFinder(prefs, (int)useRowMatchFinder);
        FIO_setAdaptMin(prefs, adaptMin);
        FIO_setAdaptMax(prefs, adaptMax);
        FIO_setRsyncable(prefs, rsyncable);
        FIO_setStreamSrcSize(prefs, streamSrcSize);
        FIO_setTargetCBlockSize(prefs, targetCBlockSize);
        FIO_setSrcSizeHint(prefs, srcSizeHint);
        FIO_setLiteralCompressionMode(prefs, literalCompressionMode);
        FIO_setSparseWrite(prefs, 0);
        if (adaptMin > cLevel) cLevel = adaptMin;
        if (adaptMax < cLevel) cLevel = adaptMax;

        /* Compare strategies constant with the ground truth */
        { ZSTD_bounds strategyBounds = ZSTD_cParam_getBounds(ZSTD_c_strategy);
          assert(ZSTD_NB_STRATEGIES == strategyBounds.upperBound);
          (void)strategyBounds; }

        if (showDefaultCParams || g_displayLevel >= 4) {
            size_t fileNb;
            for (fileNb = 0; fileNb < (size_t)filenames->tableSize; fileNb++) {
                if (showDefaultCParams)
                    printDefaultCParams(filenames->fileNames[fileNb], dictFileName, cLevel);
                if (g_displayLevel >= 4)
                    printActualCParams(filenames->fileNames[fileNb], dictFileName, cLevel, &compressionParams);
            }
        }

        if (g_displayLevel >= 4)
            FIO_displayCompressionParameters(prefs);
        if ((filenames->tableSize==1) && outFileName)
            operationResult = FIO_compressFilename(fCtx, prefs, outFileName, filenames->fileNames[0], dictFileName, cLevel, compressionParams);
        else
            operationResult = FIO_compressMultipleFilenames(fCtx, prefs, filenames->fileNames, outMirroredDirName, outDirName, outFileName, suffix, dictFileName, cLevel, compressionParams);
#else
        /* these variables are only used when compression mode is enabled */
        (void)contentSize; (void)suffix; (void)adapt; (void)rsyncable;
        (void)ultra; (void)cLevel; (void)ldmFlag; (void)literalCompressionMode;
        (void)targetCBlockSize; (void)streamSrcSize; (void)srcSizeHint;
        (void)ZSTD_strategyMap; (void)useRowMatchFinder; (void)cType;
        DISPLAYLEVEL(1, "Compression not supported \n");
#endif
    } else {  /* decompression or test */
#ifndef ZSTD_NODECOMPRESS
        if (filenames->tableSize == 1 && outFileName) {
            operationResult = FIO_decompressFilename(fCtx, prefs, outFileName, filenames->fileNames[0], dictFileName);
        } else {
            operationResult = FIO_decompressMultipleFilenames(fCtx, prefs, filenames->fileNames, outMirroredDirName, outDirName, outFileName, dictFileName);
        }
#else
        DISPLAYLEVEL(1, "Decompression not supported \n");
#endif
    }

_end:
    FIO_freePreferences(prefs);
    FIO_freeContext(fCtx);
    if (main_pause) waitEnter();
    UTIL_freeFileNamesTable(filenames);
    UTIL_freeFileNamesTable(file_of_names);
#ifndef ZSTD_NOTRACE
    TRACE_finish();
#endif

    return operationResult;
}
