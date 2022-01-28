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
#include "platform.h" /* IS_CONSOLE, PLATFORM_POSIX_VERSION */
#include "util.h"     /* UTIL_HAS_CREATEFILELIST, UTIL_createFileList */
#include <stdlib.h>   /* getenv */
#include <string.h>   /* strcmp, strlen */
#include <stdio.h>    /* fprintf(), stdin, stdout, stderr */
#include <errno.h>    /* errno */
#include <assert.h>   /* assert */

#include "fileio.h"   /* stdinmark, stdoutmark, ZSTD_EXTENSION */
#ifndef ZSTD_NOBENCH
#  include "benchzstd.h"  /* BMK_benchFiles */
#endif
#ifndef ZSTD_NODICT
#  include "dibio.h"  /* ZDICT_cover_params_t, DiB_trainFromFiles() */
#endif
#ifndef ZSTD_NOTRACE
#  include "zstdcli_trace.h"
#endif
#include "../lib/zstd.h"  /* ZSTD_VERSION_STRING, ZSTD_minCLevel, ZSTD_maxCLevel */


/*-************************************
*  Constants
**************************************/
#define COMPRESSOR_NAME "zstd command line interface"
#ifndef ZSTD_VERSION
#  define ZSTD_VERSION "v" ZSTD_VERSION_STRING
#endif
#define AUTHOR "Yann Collet"
#define WELCOME_MESSAGE "*** %s %i-bits %s, by %s ***\n", COMPRESSOR_NAME, (int)(sizeof(size_t)*8), ZSTD_VERSION, AUTHOR

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


/*-************************************
*  Command Line
**************************************/
/* print help either in `stderr` or `stdout` depending on originating request
 * error (badusage) => stderr
 * help (usage_advanced) => stdout
 */
static void usage(FILE* f, const char* programName)
{
    DISPLAY_F(f, "Usage : \n");
    DISPLAY_F(f, "      %s [args] [FILE(s)] [-o file] \n", programName);
    DISPLAY_F(f, "\n");
    DISPLAY_F(f, "FILE    : a filename \n");
    DISPLAY_F(f, "          with no FILE, or when FILE is - , read standard input\n");
    DISPLAY_F(f, "Arguments : \n");
#ifndef ZSTD_NOCOMPRESS
    DISPLAY_F(f, " -#     : # compression level (1-%d, default: %d) \n", ZSTDCLI_CLEVEL_MAX, ZSTDCLI_CLEVEL_DEFAULT);
#endif
#ifndef ZSTD_NODECOMPRESS
    DISPLAY_F(f, " -d     : decompression \n");
#endif
    DISPLAY_F(f, " -D DICT: use DICT as Dictionary for compression or decompression \n");
    DISPLAY_F(f, " -o file: result stored into `file` (only 1 output file) \n");
    DISPLAY_F(f, " -f     : disable input and output checks. Allows overwriting existing files,\n");
    DISPLAY_F(f, "          input from console, output to stdout, operating on links,\n");
    DISPLAY_F(f, "          block devices, etc.\n");
    DISPLAY_F(f, "--rm    : remove source file(s) after successful de/compression \n");
    DISPLAY_F(f, " -k     : preserve source file(s) (default) \n");
    DISPLAY_F(f, " -h/-H  : display help/long help and exit \n");
}

static void usage_advanced(const char* programName)
{
    DISPLAYOUT(WELCOME_MESSAGE);
    usage(stdout, programName);
    DISPLAYOUT( "\n");
    DISPLAYOUT( "Advanced arguments : \n");
    DISPLAYOUT( " -V     : display Version number and exit \n");

    DISPLAYOUT( " -c     : write to standard output (even if it is the console) \n");

    DISPLAYOUT( " -v     : verbose mode; specify multiple times to increase verbosity \n");
    DISPLAYOUT( " -q     : suppress warnings; specify twice to suppress errors too \n");
    DISPLAYOUT( "--[no-]progress : forcibly display, or never display the progress counter.\n");
    DISPLAYOUT( "                  note: any (de)compressed output to terminal will mix with progress counter text. \n");

#ifdef UTIL_HAS_CREATEFILELIST
    DISPLAYOUT( " -r     : operate recursively on directories \n");
    DISPLAYOUT( "--filelist FILE : read list of files to operate upon from FILE \n");
    DISPLAYOUT( "--output-dir-flat DIR : processed files are stored into DIR \n");
#endif

#ifdef UTIL_HAS_MIRRORFILELIST
    DISPLAYOUT( "--output-dir-mirror DIR : processed files are stored into DIR respecting original directory structure \n");
#endif


#ifndef ZSTD_NOCOMPRESS
    DISPLAYOUT( "--[no-]check : during compression, add XXH64 integrity checksum to frame (default: enabled)");
#ifndef ZSTD_NODECOMPRESS
    DISPLAYOUT( ". If specified with -d, decompressor will ignore/validate checksums in compressed frame (default: validate).");
#endif
#else
#ifdef ZSTD_NOCOMPRESS
    DISPLAYOUT( "--[no-]check : during decompression, ignore/validate checksums in compressed frame (default: validate).");
#endif
#endif /* ZSTD_NOCOMPRESS */

#ifndef ZSTD_NOTRACE
    DISPLAYOUT( "\n");
    DISPLAYOUT( "--trace FILE : log tracing information to FILE.");
#endif
    DISPLAYOUT( "\n");

    DISPLAYOUT( "--      : All arguments after \"--\" are treated as files \n");

#ifndef ZSTD_NOCOMPRESS
    DISPLAYOUT( "\n");
    DISPLAYOUT( "Advanced compression arguments : \n");
    DISPLAYOUT( "--ultra : enable levels beyond %i, up to %i (requires more memory) \n", ZSTDCLI_CLEVEL_MAX, ZSTD_maxCLevel());
    DISPLAYOUT( "--long[=#]: enable long distance matching with given window log (default: %u) \n", g_defaultMaxWindowLog);
    DISPLAYOUT( "--fast[=#]: switch to very fast compression levels (default: %u) \n", 1);
    DISPLAYOUT( "--adapt : dynamically adapt compression level to I/O conditions \n");
    DISPLAYOUT( "--[no-]row-match-finder : force enable/disable usage of fast row-based matchfinder for greedy, lazy, and lazy2 strategies \n");
    DISPLAYOUT( "--patch-from=FILE : specify the file to be used as a reference point for zstd's diff engine. \n");
# ifdef ZSTD_MULTITHREAD
    DISPLAYOUT( " -T#    : spawns # compression threads (default: 1, 0==# cores) \n");
    DISPLAYOUT( " -B#    : select size of each job (default: 0==automatic) \n");
    DISPLAYOUT( "--single-thread : use a single thread for both I/O and compression (result slightly different than -T1) \n");
    DISPLAYOUT( "--auto-threads={physical,logical} (default: physical} : use either physical cores or logical cores as default when specifying -T0 \n");
    DISPLAYOUT( "--rsyncable : compress using a rsync-friendly method (-B sets block size) \n");
# endif
    DISPLAYOUT( "--exclude-compressed: only compress files that are not already compressed \n");
    DISPLAYOUT( "--stream-size=# : specify size of streaming input from `stdin` \n");
    DISPLAYOUT( "--size-hint=# optimize compression parameters for streaming input of approximately this size \n");
    DISPLAYOUT( "--target-compressed-block-size=# : generate compressed block of approximately targeted size \n");
    DISPLAYOUT( "--no-dictID : don't write dictID into header (dictionary compression only) \n");
    DISPLAYOUT( "--[no-]compress-literals : force (un)compressed literals \n");

    DISPLAYOUT( "--format=zstd : compress files to the .zst format (default) \n");
#ifdef ZSTD_GZCOMPRESS
    DISPLAYOUT( "--format=gzip : compress files to the .gz format \n");
#endif
#ifdef ZSTD_LZMACOMPRESS
    DISPLAYOUT( "--format=xz : compress files to the .xz format \n");
    DISPLAYOUT( "--format=lzma : compress files to the .lzma format \n");
#endif
#ifdef ZSTD_LZ4COMPRESS
    DISPLAYOUT( "--format=lz4 : compress files to the .lz4 format \n");
#endif
#endif  /* !ZSTD_NOCOMPRESS */

#ifndef ZSTD_NODECOMPRESS
    DISPLAYOUT( "\n");
    DISPLAYOUT( "Advanced decompression arguments : \n");
    DISPLAYOUT( " -l     : print information about zstd compressed files \n");
    DISPLAYOUT( "--test  : test compressed file integrity \n");
    DISPLAYOUT( " -M#    : Set a memory usage limit for decompression \n");
# if ZSTD_SPARSE_DEFAULT
    DISPLAYOUT( "--[no-]sparse : sparse mode (default: enabled on file, disabled on stdout) \n");
# else
    DISPLAYOUT( "--[no-]sparse : sparse mode (default: disabled) \n");
# endif
#endif  /* ZSTD_NODECOMPRESS */

#ifndef ZSTD_NODICT
    DISPLAYOUT( "\n");
    DISPLAYOUT( "Dictionary builder : \n");
    DISPLAYOUT( "--train ## : create a dictionary from a training set of files \n");
    DISPLAYOUT( "--train-cover[=k=#,d=#,steps=#,split=#,shrink[=#]] : use the cover algorithm with optional args \n");
    DISPLAYOUT( "--train-fastcover[=k=#,d=#,f=#,steps=#,split=#,accel=#,shrink[=#]] : use the fast cover algorithm with optional args \n");
    DISPLAYOUT( "--train-legacy[=s=#] : use the legacy algorithm with selectivity (default: %u) \n", g_defaultSelectivityLevel);
    DISPLAYOUT( " -o DICT : DICT is dictionary name (default: %s) \n", g_defaultDictName);
    DISPLAYOUT( "--maxdict=# : limit dictionary to specified size (default: %u) \n", g_defaultMaxDictSize);
    DISPLAYOUT( "--dictID=# : force dictionary ID to specified value (default: random) \n");
#endif

#ifndef ZSTD_NOBENCH
    DISPLAYOUT( "\n");
    DISPLAYOUT( "Benchmark arguments : \n");
    DISPLAYOUT( " -b#    : benchmark file(s), using # compression level (default: %d) \n", ZSTDCLI_CLEVEL_DEFAULT);
    DISPLAYOUT( " -e#    : test all compression levels successively from -b# to -e# (default: 1) \n");
    DISPLAYOUT( " -i#    : minimum evaluation time in seconds (default: 3s) \n");
    DISPLAYOUT( " -B#    : cut file into independent blocks of size # (default: no block) \n");
    DISPLAYOUT( " -S     : output one benchmark result per input file (default: consolidated result) \n");
    DISPLAYOUT( "--priority=rt : set process priority to real-time \n");
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

/*! exeNameMatch() :
    @return : a non-zero value if exeName matches test, excluding the extension
   */
static int exeNameMatch(const char* exeName, const char* test)
{
    return !strncmp(exeName, test, strlen(test)) &&
        (exeName[strlen(test)] == '\0' || exeName[strlen(test)] == '.');
}

static void errorOut(const char* msg)
{
    DISPLAY("%s \n", msg); exit(1);
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
            DISPLAY("error: missing command argument \n"); \
            CLEAN_RETURN(1);      \
        }                         \
        ptr = argv[argNb];        \
        assert(ptr != NULL);      \
        if (ptr[0]=='-') {        \
            DISPLAY("error: command cannot be separated from its argument by another command \n"); \
            CLEAN_RETURN(1);      \
}   }   }

#define NEXT_UINT32(val32) {      \
    const char* __nb;             \
    NEXT_FIELD(__nb);             \
    val32 = readU32FromChar(&__nb); \
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
        nbWorkers = 0,
        adapt = 0,
        useRowMatchFinder = 0,
        adaptMin = MINCLEVEL,
        adaptMax = MAXCLEVEL,
        rsyncable = 0,
        nextArgumentsAreFiles = 0,
        operationResult = 0,
        separateFiles = 0,
        setRealTimePrio = 0,
        singleThread = 0,
#ifdef ZSTD_MULTITHREAD
        defaultLogicalCores = 0,
#endif
        showDefaultCParams = 0,
        ultra=0,
        contentSize=1;
    double compressibility = 0.5;
    unsigned bench_nbSeconds = 3;   /* would be better if this value was synchronized from bench */
    size_t blockSize = 0;

    FIO_prefs_t* const prefs = FIO_createPreferences();
    FIO_ctx_t* const fCtx = FIO_createContext();
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
    if ((filenames==NULL) || (file_of_names==NULL)) { DISPLAY("zstd: allocation error \n"); exit(1); }
    programName = lastNameFromPath(programName);
#ifdef ZSTD_MULTITHREAD
    nbWorkers = init_nbThreads();
#endif

    /* preset behaviors */
    if (exeNameMatch(programName, ZSTD_ZSTDMT)) nbWorkers=0, singleThread=0;
    if (exeNameMatch(programName, ZSTD_UNZSTD)) operation=zom_decompress;
    if (exeNameMatch(programName, ZSTD_CAT)) { operation=zom_decompress; FIO_overwriteMode(prefs); forceStdout=1; followLinks=1; outFileName=stdoutmark; g_displayLevel=1; }     /* supports multiple formats */
    if (exeNameMatch(programName, ZSTD_ZCAT)) { operation=zom_decompress; FIO_overwriteMode(prefs); forceStdout=1; followLinks=1; outFileName=stdoutmark; g_displayLevel=1; }    /* behave like zcat, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_GZ)) { suffix = GZ_EXTENSION; FIO_setCompressionType(prefs, FIO_gzipCompression); FIO_setRemoveSrcFile(prefs, 1); }        /* behave like gzip */
    if (exeNameMatch(programName, ZSTD_GUNZIP)) { operation=zom_decompress; FIO_setRemoveSrcFile(prefs, 1); }                                                     /* behave like gunzip, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_GZCAT)) { operation=zom_decompress; FIO_overwriteMode(prefs); forceStdout=1; followLinks=1; outFileName=stdoutmark; g_displayLevel=1; }   /* behave like gzcat, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_LZMA)) { suffix = LZMA_EXTENSION; FIO_setCompressionType(prefs, FIO_lzmaCompression); FIO_setRemoveSrcFile(prefs, 1); }    /* behave like lzma */
    if (exeNameMatch(programName, ZSTD_UNLZMA)) { operation=zom_decompress; FIO_setCompressionType(prefs, FIO_lzmaCompression); FIO_setRemoveSrcFile(prefs, 1); } /* behave like unlzma, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_XZ)) { suffix = XZ_EXTENSION; FIO_setCompressionType(prefs, FIO_xzCompression); FIO_setRemoveSrcFile(prefs, 1); }          /* behave like xz */
    if (exeNameMatch(programName, ZSTD_UNXZ)) { operation=zom_decompress; FIO_setCompressionType(prefs, FIO_xzCompression); FIO_setRemoveSrcFile(prefs, 1); }     /* behave like unxz, also supports multiple formats */
    if (exeNameMatch(programName, ZSTD_LZ4)) { suffix = LZ4_EXTENSION; FIO_setCompressionType(prefs, FIO_lz4Compression); }                                       /* behave like lz4 */
    if (exeNameMatch(programName, ZSTD_UNLZ4)) { operation=zom_decompress; FIO_setCompressionType(prefs, FIO_lz4Compression); }                                   /* behave like unlz4, also supports multiple formats */
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
                if (!strcmp(argument, "--stdout")) { forceStdout=1; outFileName=stdoutmark; g_displayLevel-=(g_displayLevel==2); continue; }
                if (!strcmp(argument, "--ultra")) { ultra=1; continue; }
                if (!strcmp(argument, "--check")) { FIO_setChecksumFlag(prefs, 2); continue; }
                if (!strcmp(argument, "--no-check")) { FIO_setChecksumFlag(prefs, 0); continue; }
                if (!strcmp(argument, "--sparse")) { FIO_setSparseWrite(prefs, 2); continue; }
                if (!strcmp(argument, "--no-sparse")) { FIO_setSparseWrite(prefs, 0); continue; }
                if (!strcmp(argument, "--test")) { operation=zom_test; continue; }
                if (!strcmp(argument, "--train")) { operation=zom_train; if (outFileName==NULL) outFileName=g_defaultDictName; continue; }
                if (!strcmp(argument, "--no-dictID")) { FIO_setDictIDFlag(prefs, 0); continue; }
                if (!strcmp(argument, "--keep")) { FIO_setRemoveSrcFile(prefs, 0); continue; }
                if (!strcmp(argument, "--rm")) { FIO_setRemoveSrcFile(prefs, 1); continue; }
                if (!strcmp(argument, "--priority=rt")) { setRealTimePrio = 1; continue; }
                if (!strcmp(argument, "--show-default-cparams")) { showDefaultCParams = 1; continue; }
                if (!strcmp(argument, "--content-size")) { contentSize = 1; continue; }
                if (!strcmp(argument, "--no-content-size")) { contentSize = 0; continue; }
                if (!strcmp(argument, "--adapt")) { adapt = 1; continue; }
                if (!strcmp(argument, "--no-row-match-finder")) { useRowMatchFinder = 1; continue; }
                if (!strcmp(argument, "--row-match-finder")) { useRowMatchFinder = 2; continue; }
                if (longCommandWArg(&argument, "--adapt=")) { adapt = 1; if (!parseAdaptParameters(argument, &adaptMin, &adaptMax)) { badusage(programName); CLEAN_RETURN(1); } continue; }
                if (!strcmp(argument, "--single-thread")) { nbWorkers = 0; singleThread = 1; continue; }
                if (!strcmp(argument, "--format=zstd")) { suffix = ZSTD_EXTENSION; FIO_setCompressionType(prefs, FIO_zstdCompression); continue; }
#ifdef ZSTD_GZCOMPRESS
                if (!strcmp(argument, "--format=gzip")) { suffix = GZ_EXTENSION; FIO_setCompressionType(prefs, FIO_gzipCompression); continue; }
#endif
#ifdef ZSTD_LZMACOMPRESS
                if (!strcmp(argument, "--format=lzma")) { suffix = LZMA_EXTENSION; FIO_setCompressionType(prefs, FIO_lzmaCompression);  continue; }
                if (!strcmp(argument, "--format=xz")) { suffix = XZ_EXTENSION; FIO_setCompressionType(prefs, FIO_xzCompression);  continue; }
#endif
#ifdef ZSTD_LZ4COMPRESS
                if (!strcmp(argument, "--format=lz4")) { suffix = LZ4_EXTENSION; FIO_setCompressionType(prefs, FIO_lz4Compression);  continue; }
#endif
                if (!strcmp(argument, "--rsyncable")) { rsyncable = 1; continue; }
                if (!strcmp(argument, "--compress-literals")) { literalCompressionMode = ZSTD_ps_enable; continue; }
                if (!strcmp(argument, "--no-compress-literals")) { literalCompressionMode = ZSTD_ps_disable; continue; }
                if (!strcmp(argument, "--no-progress")) { FIO_setProgressSetting(FIO_ps_never); continue; }
                if (!strcmp(argument, "--progress")) { FIO_setProgressSetting(FIO_ps_always); continue; }
                if (!strcmp(argument, "--exclude-compressed")) { FIO_setExcludeCompressedFile(prefs, 1); continue; }

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
                if (longCommandWArg(&argument, "--block-size=")) { blockSize = readSizeTFromChar(&argument); continue; }
                if (longCommandWArg(&argument, "--maxdict")) { NEXT_UINT32(maxDictSize); continue; }
                if (longCommandWArg(&argument, "--dictID")) { NEXT_UINT32(dictID); continue; }
                if (longCommandWArg(&argument, "--zstd=")) { if (!parseCompressionParameters(argument, &compressionParams)) { badusage(programName); CLEAN_RETURN(1); } continue; }
                if (longCommandWArg(&argument, "--stream-size=")) { streamSrcSize = readSizeTFromChar(&argument); continue; }
                if (longCommandWArg(&argument, "--target-compressed-block-size=")) { targetCBlockSize = readSizeTFromChar(&argument); continue; }
                if (longCommandWArg(&argument, "--size-hint=")) { srcSizeHint = readSizeTFromChar(&argument); continue; }
                if (longCommandWArg(&argument, "--output-dir-flat")) { NEXT_FIELD(outDirName); continue; }
#ifdef ZSTD_MULTITHREAD
                if (longCommandWArg(&argument, "--auto-threads")) {
                    const char* threadDefault = NULL;
                    NEXT_FIELD(threadDefault);
                    if (strcmp(threadDefault, "logical") == 0)
                        defaultLogicalCores = 1;
                    continue;
                }
#endif
#ifdef UTIL_HAS_MIRRORFILELIST
                if (longCommandWArg(&argument, "--output-dir-mirror")) { NEXT_FIELD(outMirroredDirName); continue; }
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
                case 'H':
                case 'h': usage_advanced(programName); CLEAN_RETURN(0);

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
                case 'c': forceStdout=1; outFileName=stdoutmark; argument++; break;

                    /* Use file content as dictionary */
                case 'D': argument++; NEXT_FIELD(dictFileName); break;

                    /* Overwrite */
                case 'f': FIO_overwriteMode(prefs); forceStdin=1; forceStdout=1; followLinks=1; allowBlockDevices=1; argument++; break;

                    /* Verbose mode */
                case 'v': g_displayLevel++; argument++; break;

                    /* Quiet mode */
                case 'q': g_displayLevel--; argument++; break;

                    /* keep source file (default) */
                case 'k': FIO_setRemoveSrcFile(prefs, 0); argument++; break;

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
                    nbWorkers = (int)readU32FromChar(&argument);
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
    if ((nbWorkers==0) && (!singleThread)) {
        /* automatically set # workers based on # of reported cpus */
        if (defaultLogicalCores) {
            nbWorkers = UTIL_countLogicalCores();
            DISPLAYLEVEL(3, "Note: %d logical core(s) detected \n", nbWorkers);
        } else {
            nbWorkers = UTIL_countPhysicalCores();
            DISPLAYLEVEL(3, "Note: %d physical core(s) detected \n", nbWorkers);
        }
    }
#else
    (void)singleThread; (void)nbWorkers;
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
        DISPLAY("file information is not supported \n");
        CLEAN_RETURN(1);
#endif
    }

    /* Check if benchmark is selected */
    if (operation==zom_bench) {
#ifndef ZSTD_NOBENCH
        benchParams.blockSize = blockSize;
        benchParams.nbWorkers = nbWorkers;
        benchParams.realTime = (unsigned)setRealTimePrio;
        benchParams.nbSeconds = bench_nbSeconds;
        benchParams.ldmFlag = ldmFlag;
        benchParams.ldmMinMatch = (int)g_ldmMinMatch;
        benchParams.ldmHashLog = (int)g_ldmHashLog;
        benchParams.useRowMatchFinder = useRowMatchFinder;
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
                        BMK_benchFilesAdvanced(&filenames->fileNames[i], 1, dictFileName, c, &compressionParams, g_displayLevel, &benchParams);
                }   }
            } else {
                for(; cLevel <= cLevelLast; cLevel++) {
                    BMK_benchFilesAdvanced(filenames->fileNames, (unsigned)filenames->tableSize, dictFileName, cLevel, &compressionParams, g_displayLevel, &benchParams);
            }   }
        } else {
            for(; cLevel <= cLevelLast; cLevel++) {
                BMK_syntheticTest(cLevel, compressibility, &compressionParams, g_displayLevel, &benchParams);
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
    if (operation==zom_test) { FIO_setTestMode(prefs, 1); outFileName=nulmark; FIO_setRemoveSrcFile(prefs, 0); }  /* test mode */
#endif

    /* No input filename ==> use stdin and stdout */
    if (filenames->tableSize == 0) UTIL_refFilename(filenames, stdinmark);
    if (!strcmp(filenames->fileNames[0], stdinmark) && !outFileName)
        outFileName = stdoutmark;  /* when input is stdin, default output is stdout */

    /* Check if input/output defined as console; trigger an error in this case */
    if (!forceStdin
     && !strcmp(filenames->fileNames[0], stdinmark)
     && IS_CONSOLE(stdin) ) {
        DISPLAYLEVEL(1, "stdin is a console, aborting\n");
        CLEAN_RETURN(1);
    }
    if ( outFileName && !strcmp(outFileName, stdoutmark)
      && IS_CONSOLE(stdout)
      && !strcmp(filenames->fileNames[0], stdinmark)
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
            DISPLAY("error : can't use --show-default-cparams in decomrpession mode \n");
            CLEAN_RETURN(1);
        }
    }

    if (dictFileName != NULL && patchFromDictFileName != NULL) {
        DISPLAY("error : can't use -D and --patch-from=# at the same time \n");
        CLEAN_RETURN(1);
    }

    if (patchFromDictFileName != NULL && filenames->tableSize > 1) {
        DISPLAY("error : can't use --patch-from=# on multiple files \n");
        CLEAN_RETURN(1);
    }

    /* No status message in pipe mode (stdin - stdout) */
    hasStdout = outFileName && !strcmp(outFileName,stdoutmark);

    if ((hasStdout || !IS_CONSOLE(stderr)) && (g_displayLevel==2)) g_displayLevel=1;

    /* IO Stream/File */
    FIO_setHasStdoutOutput(fCtx, hasStdout);
    FIO_setNbFilesTotal(fCtx, (int)filenames->tableSize);
    FIO_determineHasStdinInput(fCtx, filenames);
    FIO_setNotificationLevel(g_displayLevel);
    FIO_setAllowBlockDevices(prefs, allowBlockDevices);
    FIO_setPatchFromMode(prefs, patchFromDictFileName != NULL);
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
        FIO_setContentSize(prefs, contentSize);
        FIO_setNbWorkers(prefs, nbWorkers);
        FIO_setBlockSize(prefs, (int)blockSize);
        if (g_overlapLog!=OVERLAP_LOG_DEFAULT) FIO_setOverlapLog(prefs, (int)g_overlapLog);
        FIO_setLdmFlag(prefs, (unsigned)ldmFlag);
        FIO_setLdmHashLog(prefs, (int)g_ldmHashLog);
        FIO_setLdmMinMatch(prefs, (int)g_ldmMinMatch);
        if (g_ldmBucketSizeLog != LDM_PARAM_DEFAULT) FIO_setLdmBucketSizeLog(prefs, (int)g_ldmBucketSizeLog);
        if (g_ldmHashRateLog != LDM_PARAM_DEFAULT) FIO_setLdmHashRateLog(prefs, (int)g_ldmHashRateLog);
        FIO_setAdaptiveMode(prefs, (unsigned)adapt);
        FIO_setUseRowMatchFinder(prefs, useRowMatchFinder);
        FIO_setAdaptMin(prefs, adaptMin);
        FIO_setAdaptMax(prefs, adaptMax);
        FIO_setRsyncable(prefs, rsyncable);
        FIO_setStreamSrcSize(prefs, streamSrcSize);
        FIO_setTargetCBlockSize(prefs, targetCBlockSize);
        FIO_setSrcSizeHint(prefs, srcSizeHint);
        FIO_setLiteralCompressionMode(prefs, literalCompressionMode);
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
        (void)contentSize; (void)suffix; (void)adapt; (void)rsyncable; (void)ultra; (void)cLevel; (void)ldmFlag; (void)literalCompressionMode; (void)targetCBlockSize; (void)streamSrcSize; (void)srcSizeHint; (void)ZSTD_strategyMap; (void)useRowMatchFinder; /* not used when ZSTD_NOCOMPRESS set */
        DISPLAY("Compression not supported \n");
#endif
    } else {  /* decompression or test */
#ifndef ZSTD_NODECOMPRESS
        if (filenames->tableSize == 1 && outFileName) {
            operationResult = FIO_decompressFilename(fCtx, prefs, outFileName, filenames->fileNames[0], dictFileName);
        } else {
            operationResult = FIO_decompressMultipleFilenames(fCtx, prefs, filenames->fileNames, outMirroredDirName, outDirName, outFileName, dictFileName);
        }
#else
        DISPLAY("Decompression not supported \n");
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
