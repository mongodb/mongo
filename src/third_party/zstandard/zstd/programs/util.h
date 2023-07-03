/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef UTIL_H_MODULE
#define UTIL_H_MODULE

#if defined (__cplusplus)
extern "C" {
#endif


/*-****************************************
*  Dependencies
******************************************/
#include "platform.h"     /* PLATFORM_POSIX_VERSION, ZSTD_NANOSLEEP_SUPPORT, ZSTD_SETPRIORITY_SUPPORT */
#include <stddef.h>       /* size_t, ptrdiff_t */
#include <sys/types.h>    /* stat, utime */
#include <sys/stat.h>     /* stat, chmod */
#include "../lib/common/mem.h"          /* U64 */


/*-************************************************************
* Avoid fseek()'s 2GiB barrier with MSVC, macOS, *BSD, MinGW
***************************************************************/
#if defined(_MSC_VER) && (_MSC_VER >= 1400)
#  define UTIL_fseek _fseeki64
#elif !defined(__64BIT__) && (PLATFORM_POSIX_VERSION >= 200112L) /* No point defining Large file for 64 bit */
#  define UTIL_fseek fseeko
#elif defined(__MINGW32__) && defined(__MSVCRT__) && !defined(__STRICT_ANSI__) && !defined(__NO_MINGW_LFS)
#  define UTIL_fseek fseeko64
#else
#  define UTIL_fseek fseek
#endif


/*-*************************************************
*  Sleep & priority functions: Windows - Posix - others
***************************************************/
#if defined(_WIN32)
#  include <windows.h>
#  define SET_REALTIME_PRIORITY SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)
#  define UTIL_sleep(s) Sleep(1000*s)
#  define UTIL_sleepMilli(milli) Sleep(milli)

#elif PLATFORM_POSIX_VERSION > 0 /* Unix-like operating system */
#  include <unistd.h>   /* sleep */
#  define UTIL_sleep(s) sleep(s)
#  if ZSTD_NANOSLEEP_SUPPORT   /* necessarily defined in platform.h */
#      define UTIL_sleepMilli(milli) { struct timespec t; t.tv_sec=0; t.tv_nsec=milli*1000000ULL; nanosleep(&t, NULL); }
#  else
#      define UTIL_sleepMilli(milli) /* disabled */
#  endif
#  if ZSTD_SETPRIORITY_SUPPORT
#    include <sys/resource.h> /* setpriority */
#    define SET_REALTIME_PRIORITY setpriority(PRIO_PROCESS, 0, -20)
#  else
#    define SET_REALTIME_PRIORITY /* disabled */
#  endif

#else  /* unknown non-unix operating system */
#  define UTIL_sleep(s)          /* disabled */
#  define UTIL_sleepMilli(milli) /* disabled */
#  define SET_REALTIME_PRIORITY  /* disabled */
#endif


/*-****************************************
*  Compiler specifics
******************************************/
#if defined(__INTEL_COMPILER)
#  pragma warning(disable : 177)    /* disable: message #177: function was declared but never referenced, useful with UTIL_STATIC */
#endif
#if defined(__GNUC__)
#  define UTIL_STATIC static __attribute__((unused))
#elif defined (__cplusplus) || (defined (__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) /* C99 */)
#  define UTIL_STATIC static inline
#elif defined(_MSC_VER)
#  define UTIL_STATIC static __inline
#else
#  define UTIL_STATIC static  /* this version may generate warnings for unused static functions; disable the relevant warning */
#endif


/*-****************************************
*  Console log
******************************************/
extern int g_utilDisplayLevel;

/**
 * Displays a message prompt and returns success (0) if first character from stdin
 * matches any from acceptableLetters. Otherwise, returns failure (1) and displays abortMsg.
 * If any of the inputs are stdin itself, then automatically return failure (1).
 */
int UTIL_requireUserConfirmation(const char* prompt, const char* abortMsg, const char* acceptableLetters, int hasStdinInput);


/*-****************************************
*  File functions
******************************************/
#if defined(_MSC_VER)
    typedef struct __stat64 stat_t;
    typedef int mode_t;
#elif defined(__MINGW32__) && defined (__MSVCRT__)
    typedef struct _stati64 stat_t;
#else
    typedef struct stat stat_t;
#endif

#if defined(_MSC_VER) || defined(__MINGW32__) || defined (__MSVCRT__) /* windows support */
#define PATH_SEP '\\'
#define STRDUP(s) _strdup(s)
#else
#define PATH_SEP '/'
#include <libgen.h>
#define STRDUP(s) strdup(s)
#endif


/**
 * Calls platform's equivalent of stat() on filename and writes info to statbuf.
 * Returns success (1) or failure (0).
 *
 * UTIL_fstat() is like UTIL_stat() but takes an optional fd that refers to the
 * file in question. It turns out that this can be meaningfully faster. If fd is
 * -1, behaves just like UTIL_stat() (i.e., falls back to using the filename).
 */
int UTIL_stat(const char* filename, stat_t* statbuf);
int UTIL_fstat(const int fd, const char* filename, stat_t* statbuf);

/**
 * Instead of getting a file's stats, this updates them with the info in the
 * provided stat_t. Currently sets owner, group, atime, and mtime. Will only
 * update this info for regular files.
 *
 * UTIL_setFDStat() also takes an fd, and will preferentially use that to
 * indicate which file to modify, If fd is -1, it will fall back to using the
 * filename.
 */
int UTIL_setFileStat(const char* filename, const stat_t* statbuf);
int UTIL_setFDStat(const int fd, const char* filename, const stat_t* statbuf);

/**
 * Set atime to now and mtime to the st_mtim in statbuf.
 *
 * Directly wraps utime() or utimensat(). Returns -1 on error.
 * Does not validate filename is valid.
 */
int UTIL_utime(const char* filename, const stat_t *statbuf);

/*
 * These helpers operate on a pre-populated stat_t, i.e., the result of
 * calling one of the above functions.
 */

int UTIL_isRegularFileStat(const stat_t* statbuf);
int UTIL_isDirectoryStat(const stat_t* statbuf);
int UTIL_isFIFOStat(const stat_t* statbuf);
int UTIL_isBlockDevStat(const stat_t* statbuf);
U64 UTIL_getFileSizeStat(const stat_t* statbuf);

/**
 * Like chmod(), but only modifies regular files. Provided statbuf may be NULL,
 * in which case this function will stat() the file internally, in order to
 * check whether it should be modified.
 *
 * If fd is -1, fd is ignored and the filename is used.
 */
int UTIL_chmod(char const* filename, const stat_t* statbuf, mode_t permissions);
int UTIL_fchmod(const int fd, char const* filename, const stat_t* statbuf, mode_t permissions);

/*
 * In the absence of a pre-existing stat result on the file in question, these
 * functions will do a stat() call internally and then use that result to
 * compute the needed information.
 */

int UTIL_isRegularFile(const char* infilename);
int UTIL_isDirectory(const char* infilename);
int UTIL_isSameFile(const char* file1, const char* file2);
int UTIL_isSameFileStat(const char* file1, const char* file2, const stat_t* file1Stat, const stat_t* file2Stat);
int UTIL_isCompressedFile(const char* infilename, const char *extensionList[]);
int UTIL_isLink(const char* infilename);
int UTIL_isFIFO(const char* infilename);

/**
 * Returns with the given file descriptor is a console.
 * Allows faking whether stdin/stdout/stderr is a console
 * using UTIL_fake*IsConsole().
 */
int UTIL_isConsole(FILE* file);

/**
 * Pretends that stdin/stdout/stderr is a console for testing.
 */
void UTIL_fakeStdinIsConsole(void);
void UTIL_fakeStdoutIsConsole(void);
void UTIL_fakeStderrIsConsole(void);

/**
 * Emit traces for functions that read, or modify file metadata.
 */
void UTIL_traceFileStat(void);

#define UTIL_FILESIZE_UNKNOWN  ((U64)(-1))
U64 UTIL_getFileSize(const char* infilename);
U64 UTIL_getTotalFileSize(const char* const * fileNamesTable, unsigned nbFiles);

/**
 * Take @size in bytes,
 * prepare the components to pretty-print it in a scaled way.
 * The components in the returned struct should be passed in
 * precision, value, suffix order to a "%.*f%s" format string.
 * Output policy is sensible to @g_utilDisplayLevel,
 * for verbose mode (@g_utilDisplayLevel >= 4),
 * does not scale down.
 */
typedef struct {
  double value;
  int precision;
  const char* suffix;
} UTIL_HumanReadableSize_t;

UTIL_HumanReadableSize_t UTIL_makeHumanReadableSize(U64 size);

int UTIL_compareStr(const void *p1, const void *p2);
const char* UTIL_getFileExtension(const char* infilename);
void  UTIL_mirrorSourceFilesDirectories(const char** fileNamesTable, unsigned int nbFiles, const char *outDirName);
char* UTIL_createMirroredDestDirName(const char* srcFileName, const char* outDirRootName);



/*-****************************************
 *  Lists of Filenames
 ******************************************/

typedef struct
{   const char** fileNames;
    char* buf;            /* fileNames are stored in this buffer (or are read-only) */
    size_t tableSize;     /* nb of fileNames */
    size_t tableCapacity;
} FileNamesTable;

/*! UTIL_createFileNamesTable_fromFileName() :
 *  read filenames from @inputFileName, and store them into returned object.
 * @return : a FileNamesTable*, or NULL in case of error (ex: @inputFileName doesn't exist).
 *  Note: inputFileSize must be less than 50MB
 */
FileNamesTable*
UTIL_createFileNamesTable_fromFileName(const char* inputFileName);

/*! UTIL_assembleFileNamesTable() :
 *  This function takes ownership of its arguments, @filenames and @buf,
 *  and store them inside the created object.
 *  note : this function never fails,
 *         it will rather exit() the program if internal allocation fails.
 * @return : resulting FileNamesTable* object.
 */
FileNamesTable*
UTIL_assembleFileNamesTable(const char** filenames, size_t tableSize, char* buf);

/*! UTIL_freeFileNamesTable() :
 *  This function is compatible with NULL argument and never fails.
 */
void UTIL_freeFileNamesTable(FileNamesTable* table);

/*! UTIL_mergeFileNamesTable():
 * @return : FileNamesTable*, concatenation of @table1 and @table2
 *  note: @table1 and @table2 are consumed (freed) by this operation
 */
FileNamesTable*
UTIL_mergeFileNamesTable(FileNamesTable* table1, FileNamesTable* table2);


/*! UTIL_expandFNT() :
 *  read names from @fnt, and expand those corresponding to directories
 *  update @fnt, now containing only file names,
 *  note : in case of error, @fnt[0] is NULL
 */
void UTIL_expandFNT(FileNamesTable** fnt, int followLinks);

/*! UTIL_createFNT_fromROTable() :
 *  copy the @filenames pointer table inside the returned object.
 *  The names themselves are still stored in their original buffer, which must outlive the object.
 * @return : a FileNamesTable* object,
 *        or NULL in case of error
 */
FileNamesTable*
UTIL_createFNT_fromROTable(const char** filenames, size_t nbFilenames);

/*! UTIL_allocateFileNamesTable() :
 *  Allocates a table of const char*, to insert read-only names later on.
 *  The created FileNamesTable* doesn't hold a buffer.
 * @return : FileNamesTable*, or NULL, if allocation fails.
 */
FileNamesTable* UTIL_allocateFileNamesTable(size_t tableSize);

/*! UTIL_searchFileNamesTable() :
 *  Searched through entries in FileNamesTable for a specific name.
 * @return : index of entry if found or -1 if not found
 */
int UTIL_searchFileNamesTable(FileNamesTable* table, char const* name);

/*! UTIL_refFilename() :
 *  Add a reference to read-only name into @fnt table.
 *  As @filename is only referenced, its lifetime must outlive @fnt.
 *  Internal table must be large enough to reference a new member,
 *  otherwise its UB (protected by an `assert()`).
 */
void UTIL_refFilename(FileNamesTable* fnt, const char* filename);


/* UTIL_createExpandedFNT() is only active if UTIL_HAS_CREATEFILELIST is defined.
 * Otherwise, UTIL_createExpandedFNT() is a shell function which does nothing
 * apart from displaying a warning message.
 */
#ifdef _WIN32
#  define UTIL_HAS_CREATEFILELIST
#elif defined(__linux__) || (PLATFORM_POSIX_VERSION >= 200112L)  /* opendir, readdir require POSIX.1-2001 */
#  define UTIL_HAS_CREATEFILELIST
#  define UTIL_HAS_MIRRORFILELIST
#else
   /* do not define UTIL_HAS_CREATEFILELIST */
#endif

/*! UTIL_createExpandedFNT() :
 *  read names from @filenames, and expand those corresponding to directories.
 *  links are followed or not depending on @followLinks directive.
 * @return : an expanded FileNamesTable*, where each name is a file
 *        or NULL in case of error
 */
FileNamesTable*
UTIL_createExpandedFNT(const char* const* filenames, size_t nbFilenames, int followLinks);

#if defined(_WIN32) || defined(WIN32)
DWORD CountSetBits(ULONG_PTR bitMask);
#endif

/*-****************************************
 *  System
 ******************************************/

int UTIL_countCores(int logical);

int UTIL_countPhysicalCores(void);

int UTIL_countLogicalCores(void);

#if defined (__cplusplus)
}
#endif

#endif /* UTIL_H_MODULE */
